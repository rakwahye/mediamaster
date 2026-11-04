/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file player.c
 * @brief Playback core.
 *
 * Flush protocol. Only the consumer may move the play ring's tail, so the
 * control side never touches it. To discard queued audio, control first
 * stops the reader (head is then stable), publishes `drop_to = head` and
 * bumps `drop_seq`. The sink's pull sees the new sequence, skips the tail
 * to drop_to and discards markers before it. Anything written after
 * drop_to (the next stream's audio, its START marker) survives, so control
 * never has to wait for the sink. */

#include "player.h"
#include "config.h"
#include "ipc.h"
#include "loader.h"
#include "log.h"
#include "ring.h"
#include "mm_plugin.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "player"
#define CH MM_PCM_CHANNELS
#define READ_CHUNK 1024u
#define FULL_WAIT_NS (5 * 1000 * 1000)

static struct {
	pthread_mutex_t lock; /* control-side state below, up to the RT block */
	int inited;

	const struct mm_plugin *sink_p;
	void *sink_priv;

	const struct mm_plugin *src_p;
	void *stream;
	struct mm_stream_info info;
	char uri[1024];
	uint32_t item;
	int state;
	int vol;

	pthread_t reader;
	int reader_on;
	atomic_int reader_stop;
	atomic_int reader_done;

	/* Shared with the sink thread. */
	struct mm_ring play, tap;
	struct mm_markq marks;
	atomic_int paused;
	atomic_uint gain_bits;       /* float target gain */
	atomic_uint_least64_t drop_to;
	atomic_uint drop_seq;
	atomic_uint cur_item;        /* item at the speaker */
	atomic_uint ended_item;      /* last item whose END was reached */
	atomic_uint_least64_t item_frames; /* frames into cur_item */
	atomic_uint_least64_t underruns;
	atomic_int tap_on;
	atomic_uint_least64_t tap_overruns;

	/* Sink thread only. */
	unsigned seen_drop_seq;
	float cur_gain;
	int flowing; /* audio of the current item has reached the sink */
} P = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ------------------------------------------------------------------ RT side */

static float bits_to_f(unsigned b) { float f; memcpy(&f, &b, sizeof(f)); return f; }
static unsigned f_to_bits(float f) { unsigned b; memcpy(&b, &f, sizeof(b)); return b; }

static void apply_marker(const struct mm_mark *m)
{
	if (m->kind == MM_MARK_START) {
		atomic_store_explicit(&P.cur_item, m->item, memory_order_relaxed);
		atomic_store_explicit(&P.item_frames, m->offset, memory_order_relaxed);
		P.flowing = 0; /* startup latency after play/seek is not an underrun */
	} else if (m->kind == MM_MARK_END) {
		atomic_store_explicit(&P.ended_item, m->item, memory_order_release);
	}
}

static void pull(void *ctx, float *dst, uint32_t frames)
{
	(void)ctx;
	unsigned seq = atomic_load_explicit(&P.drop_seq, memory_order_acquire);
	if (seq != P.seen_drop_seq) {
		uint64_t to = atomic_load_explicit(&P.drop_to, memory_order_relaxed);
		ring_skip_to(&P.play, to);
		struct mm_mark m;
		while (markq_peek(&P.marks, &m) && (int64_t)(m.pos - to) < 0) markq_pop(&P.marks);
		P.seen_drop_seq = seq;
		P.flowing = 0;
	}

	if (atomic_load_explicit(&P.paused, memory_order_relaxed)) {
		memset(dst, 0, (size_t)frames * CH * sizeof(float));
		return;
	}

	uint32_t done = 0;
	while (done < frames) {
		uint32_t want = frames - done;
		struct mm_mark m;
		if (markq_peek(&P.marks, &m)) {
			uint64_t tail = ring_tail(&P.play);
			if ((int64_t)(m.pos - tail) <= 0) {
				apply_marker(&m);
				markq_pop(&P.marks);
				continue;
			}
			if (m.pos - tail < want) want = (uint32_t)(m.pos - tail);
		}
		uint32_t got = ring_read(&P.play, dst + (size_t)done * CH, want);
		if (!got) break;
		atomic_fetch_add_explicit(&P.item_frames, got, memory_order_relaxed);
		done += got;
		P.flowing = 1;
	}
	if (done < frames) {
		memset(dst + (size_t)done * CH, 0, (size_t)(frames - done) * CH * sizeof(float));
		/* A gap after audio started, before the item's end. */
		unsigned cur = atomic_load_explicit(&P.cur_item, memory_order_relaxed);
		if (P.flowing && cur && cur != atomic_load_explicit(&P.ended_item, memory_order_relaxed))
			atomic_fetch_add_explicit(&P.underruns, 1, memory_order_relaxed);
	}

	/* Tap before gain: identification must work at any volume. */
	if (atomic_load_explicit(&P.tap_on, memory_order_relaxed) && done) {
		uint32_t w = ring_write(&P.tap, dst, done);
		if (w < done) atomic_fetch_add_explicit(&P.tap_overruns, 1, memory_order_relaxed);
	}

	/* Linear ramp from the last gain to the target over this buffer. */
	float target = bits_to_f(atomic_load_explicit(&P.gain_bits, memory_order_relaxed));
	float g = P.cur_gain;
	if (g == target) {
		if (g != 1.0f)
			for (size_t i = 0; i < (size_t)frames * CH; i++) dst[i] *= g;
	} else {
		float step = (target - g) / (float)frames;
		for (uint32_t f = 0; f < frames; f++) {
			g += step;
			for (uint32_t c = 0; c < CH; c++) dst[(size_t)f * CH + c] *= g;
		}
		P.cur_gain = target;
	}
}

/* ------------------------------------------------------------------ reader */

static void sleep_ns(long ns)
{
	struct timespec ts = { 0, ns };
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

static void *reader_main(void *arg)
{
	(void)arg;
	static float buf[READ_CHUNK * CH]; /* one reader at a time */
	const struct mm_source_ops *src = P.src_p->source;
	void *stream = P.stream;

	while (!atomic_load(&P.reader_stop)) {
		long n = src->read(stream, buf, READ_CHUNK);
		if (n < 0) {
			LOGW(TAG, "%s: read error %ld", P.uri, n);
			break;
		}
		if (n == 0) break;
		if (n > (long)READ_CHUNK) n = READ_CHUNK;
		uint32_t off = 0;
		while (off < (uint32_t)n && !atomic_load(&P.reader_stop)) {
			off += ring_write(&P.play, buf + (size_t)off * CH, (uint32_t)n - off);
			if (off < (uint32_t)n) sleep_ns(FULL_WAIT_NS);
		}
	}
	if (!atomic_load(&P.reader_stop)) {
		struct mm_mark m = { .pos = ring_head(&P.play), .item = P.item, .kind = MM_MARK_END };
		if (markq_push(&P.marks, &m) != 0) LOGW(TAG, "marker queue full");
	}
	atomic_store(&P.reader_done, 1);
	return NULL;
}

/* Lock held. */
static int reader_start(void)
{
	atomic_store(&P.reader_stop, 0);
	atomic_store(&P.reader_done, 0);
	int err = pthread_create(&P.reader, NULL, reader_main, NULL);
	if (err) {
		LOGE(TAG, "reader thread: %s", strerror(err));
		return -1;
	}
	P.reader_on = 1;
	return 0;
}

/* Lock held. Interrupts a blocked read. */
static void reader_join(void)
{
	if (!P.reader_on) return;
	atomic_store(&P.reader_stop, 1);
	if (P.src_p->source->interrupt) P.src_p->source->interrupt(P.stream);
	pthread_join(P.reader, NULL);
	P.reader_on = 0;
}

/* Lock held, reader stopped. Discard everything queued so far. */
static void flush(void)
{
	atomic_store_explicit(&P.drop_to, ring_head(&P.play), memory_order_relaxed);
	atomic_fetch_add_explicit(&P.drop_seq, 1, memory_order_release);
}

/* Lock held. */
static void close_stream(void)
{
	reader_join();
	if (P.stream) {
		P.src_p->source->close(P.stream);
		P.stream = NULL;
	}
	P.src_p = NULL;
	P.uri[0] = '\0';
	memset(&P.info, 0, sizeof(P.info));
}

static int push_start(uint64_t offset_frames)
{
	struct mm_mark m = { .pos = ring_head(&P.play), .offset = offset_frames, .item = P.item, .kind = MM_MARK_START };
	return markq_push(&P.marks, &m);
}

/* ------------------------------------------------------------------ control */

/* "scheme:rest" → scheme; a bare path is "file". */
static void uri_scheme(const char *uri, char *out, size_t cap)
{
	const char *c = strchr(uri, ':');
	size_t n = c ? (size_t)(c - uri) : 0;
	int ok = n > 0 && n < cap;
	for (size_t i = 0; ok && i < n; i++) {
		char ch = uri[i];
		ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		     (i && ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.'));
	}
	if (ok) {
		memcpy(out, uri, n);
		out[n] = '\0';
	} else {
		snprintf(out, cap, "file");
	}
}

int player_play(const char *uri, char *err, size_t errlen)
{
	char scheme[32];
	const struct mm_plugin *p;
	void *priv, *stream = NULL;
	struct mm_stream_info info = { 0, 0 };

	uri_scheme(uri, scheme, sizeof(scheme));
	if (loader_find_source(scheme, &p, &priv) != 0) {
		snprintf(err, errlen, "no source for '%s:'", scheme);
		return -1;
	}

	pthread_mutex_lock(&P.lock);
	if (!P.inited) {
		pthread_mutex_unlock(&P.lock);
		snprintf(err, errlen, "no sink");
		return -1;
	}
	/* Open before stopping what plays, so a bad URI leaves it alone. */
	if (p->source->open(priv, uri, &stream, &info) != 0 || !stream) {
		pthread_mutex_unlock(&P.lock);
		snprintf(err, errlen, "%s could not open %s", p->name, uri);
		return -1;
	}
	close_stream();
	flush();
	P.src_p = p;
	P.stream = stream;
	P.info = info;
	snprintf(P.uri, sizeof(P.uri), "%s", uri);
	P.item++;
	if (P.item == 0) P.item = 1;
	push_start(0);
	atomic_store(&P.paused, 0);
	int rc = reader_start();
	if (rc != 0) {
		close_stream();
		snprintf(err, errlen, "could not start reader");
	}
	P.state = rc ? PLAYER_STOPPED : PLAYER_PLAYING;
	pthread_mutex_unlock(&P.lock);
	if (!rc) LOGI(TAG, "playing %s (%s)", uri, p->name);
	return rc;
}

void player_stop(void)
{
	pthread_mutex_lock(&P.lock);
	close_stream();
	flush();
	atomic_store(&P.cur_item, 0);
	atomic_store(&P.paused, 0);
	P.state = PLAYER_STOPPED;
	pthread_mutex_unlock(&P.lock);
}

int player_pause(int on)
{
	pthread_mutex_lock(&P.lock);
	if (P.state == PLAYER_STOPPED) {
		pthread_mutex_unlock(&P.lock);
		return 0;
	}
	int now = on < 0 ? !atomic_load(&P.paused) : !!on;
	atomic_store(&P.paused, now);
	P.state = now ? PLAYER_PAUSED : PLAYER_PLAYING;
	pthread_mutex_unlock(&P.lock);
	return now;
}

uint64_t player_position_ms(void)
{
	if (!atomic_load(&P.cur_item)) return 0;
	return atomic_load(&P.item_frames) * 1000u / MM_PCM_RATE;
}

int player_seek_ms(int64_t ms, int relative, char *err, size_t errlen)
{
	pthread_mutex_lock(&P.lock);
	int rc = -1;
	if (P.state == PLAYER_STOPPED || !P.stream) {
		snprintf(err, errlen, "nothing playing");
		goto out;
	}
	if (!(P.info.flags & MM_STREAM_SEEKABLE) || !P.src_p->source->seek) {
		snprintf(err, errlen, "not seekable");
		goto out;
	}
	int64_t target = relative ? (int64_t)player_position_ms() + ms : ms;
	if (target < 0) target = 0;
	if (P.info.duration_ms && (uint64_t)target > P.info.duration_ms) target = (int64_t)P.info.duration_ms;

	reader_join();
	if (P.src_p->source->seek(P.stream, (uint64_t)target) != 0) {
		snprintf(err, errlen, "seek failed");
		/* Keep playing from wherever the source is now. */
		flush();
		push_start(atomic_load(&P.item_frames));
		reader_start();
		goto out;
	}
	flush();
	push_start((uint64_t)target * MM_PCM_RATE / 1000u);
	rc = reader_start();
	if (rc) snprintf(err, errlen, "could not restart reader");
out:
	pthread_mutex_unlock(&P.lock);
	return rc;
}

void player_set_volume(int pct)
{
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	float v = (float)pct / 100.0f;
	pthread_mutex_lock(&P.lock);
	P.vol = pct;
	atomic_store(&P.gain_bits, f_to_bits(v * v * v)); /* roughly perceptual */
	pthread_mutex_unlock(&P.lock);
}

int player_volume(void)
{
	pthread_mutex_lock(&P.lock);
	int v = P.vol;
	pthread_mutex_unlock(&P.lock);
	return v;
}

int player_state(void)
{
	pthread_mutex_lock(&P.lock);
	int s = P.state;
	pthread_mutex_unlock(&P.lock);
	return s;
}

void player_tick(void)
{
	pthread_mutex_lock(&P.lock);
	if (P.reader_on && atomic_load(&P.reader_done) &&
	    atomic_load_explicit(&P.ended_item, memory_order_acquire) == P.item) {
		LOGI(TAG, "finished %s", P.uri);
		close_stream();
		atomic_store(&P.cur_item, 0);
		P.state = PLAYER_STOPPED;
	}
	pthread_mutex_unlock(&P.lock);
}

uint32_t player_buffered_frames(void) { return ring_fill(&P.play); }
uint64_t player_underruns(void) { return atomic_load(&P.underruns); }

void player_tap_enable(int on) { atomic_store(&P.tap_on, on); }
uint32_t player_tap_read(float *dst, uint32_t frames) { return ring_read(&P.tap, dst, frames); }
uint64_t player_tap_overruns(void) { return atomic_load(&P.tap_overruns); }

/* ------------------------------------------------------------------ verbs */

static int v_play(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	if (c->argc < 2) {
		snprintf(c->reply, c->reply_len, "usage: play <uri>");
		return MM_ERR;
	}
	char uri[1024];
	size_t off = 0;
	for (int i = 1; i < c->argc && off < sizeof(uri); i++)
		off += (size_t)snprintf(uri + off, sizeof(uri) - off, "%s%s", i > 1 ? " " : "", c->argv[i]);
	char err[256];
	if (player_play(uri, err, sizeof(err)) != 0) {
		snprintf(c->reply, c->reply_len, "%s", err);
		return MM_ERR;
	}
	snprintf(c->reply, c->reply_len, "playing %s", uri);
	return MM_OK;
}

static int v_stop(void *priv, struct mm_verb_ctx *c)
{
	(void)priv; (void)c;
	player_stop();
	return MM_OK;
}

static int v_pause_common(struct mm_verb_ctx *c, int mode)
{
	if (player_state() == PLAYER_STOPPED) {
		snprintf(c->reply, c->reply_len, "nothing playing");
		return MM_ERR;
	}
	snprintf(c->reply, c->reply_len, "%s", player_pause(mode) ? "paused" : "playing");
	return MM_OK;
}
static int v_pause(void *priv, struct mm_verb_ctx *c) { (void)priv; return v_pause_common(c, 1); }
static int v_resume(void *priv, struct mm_verb_ctx *c) { (void)priv; return v_pause_common(c, 0); }
static int v_toggle(void *priv, struct mm_verb_ctx *c) { (void)priv; return v_pause_common(c, -1); }

/* "90", "1:30", "+10", "-5" (seconds, may be fractional). */
static int parse_time(const char *s, int64_t *ms, int *relative)
{
	*relative = (*s == '+' || *s == '-');
	int neg = *s == '-';
	if (*relative) s++;
	char *end;
	double a = strtod(s, &end), total = a;
	if (end == s) return -1;
	if (*end == ':') {
		const char *s2 = end + 1;
		double b = strtod(s2, &end);
		if (end == s2) return -1;
		total = a * 60.0 + b;
	}
	if (*end || total < 0) return -1;
	*ms = (int64_t)(total * 1000.0 + 0.5);
	if (neg) *ms = -*ms;
	return 0;
}

static int v_seek(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	int64_t ms;
	int rel;
	if (c->argc != 2 || parse_time(c->argv[1], &ms, &rel) != 0) {
		snprintf(c->reply, c->reply_len, "usage: seek <sec | m:ss | +sec | -sec>");
		return MM_ERR;
	}
	char err[128];
	if (player_seek_ms(ms, rel, err, sizeof(err)) != 0) {
		snprintf(c->reply, c->reply_len, "%s", err);
		return MM_ERR;
	}
	return MM_OK;
}

static int v_vol(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	if (c->argc == 2) {
		const char *s = c->argv[1];
		int rel = (*s == '+' || *s == '-');
		char *end;
		long v = strtol(s, &end, 10);
		if (end == s || *end) {
			snprintf(c->reply, c->reply_len, "usage: vol [0-100 | +n | -n]");
			return MM_ERR;
		}
		player_set_volume((int)(rel ? player_volume() + v : v));
	}
	snprintf(c->reply, c->reply_len, "%d", player_volume());
	return MM_OK;
}

static int v_status(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	static const char *const names[] = { "stopped", "playing", "paused" };
	pthread_mutex_lock(&P.lock);
	uint64_t pos = player_position_ms();
	char dur[32] = "-";
	if (P.info.duration_ms)
		snprintf(dur, sizeof(dur), "%.3f", (double)P.info.duration_ms / 1000.0);
	snprintf(c->reply, c->reply_len,
	         "state: %s\nuri: %s\nsource: %s\nposition: %.3f\nduration: %s\nseekable: %s\n"
	         "volume: %d\nsink: %s\nbuffered: %.3f\nunderruns: %" PRIu64 "\n",
	         names[P.state], P.uri[0] ? P.uri : "-", P.src_p ? P.src_p->name : "-",
	         (double)pos / 1000.0, dur,
	         (P.info.flags & MM_STREAM_SEEKABLE) ? "yes" : "no",
	         P.vol, P.sink_p ? P.sink_p->name : "-",
	         (double)ring_fill(&P.play) / MM_PCM_RATE,
	         (uint64_t)atomic_load(&P.underruns));
	pthread_mutex_unlock(&P.lock);
	return MM_REPLY_RAW;
}

static int register_verbs(void)
{
	int rc = 0;
	rc |= ipc_register("player", "play", "<uri>\tplay a URI (sine:440, raw:/path.f32, ...)", v_play, NULL);
	rc |= ipc_register("player", "stop", "\tstop and clear the buffer", v_stop, NULL);
	rc |= ipc_register("player", "pause", "\tpause", v_pause, NULL);
	rc |= ipc_register("player", "resume", "\tresume", v_resume, NULL);
	rc |= ipc_register("player", "toggle", "\tpause/resume", v_toggle, NULL);
	rc |= ipc_register("player", "seek", "<s|m:ss|+s|-s>\tseek, absolute or relative", v_seek, NULL);
	rc |= ipc_register("player", "vol", "[0-100|+n|-n]\tget or set volume", v_vol, NULL);
	rc |= ipc_register("player", "status", "\tplayback state", v_status, NULL);
	return rc;
}

/* ------------------------------------------------------------------ lifecycle */

static int start_sink(void)
{
	char list[256];
	snprintf(list, sizeof(list), "%s", config_get_str("host.sink", "pipewire,null"));
	char *save = NULL;
	for (char *n = strtok_r(list, ", \t", &save); n; n = strtok_r(NULL, ", \t", &save)) {
		const struct mm_plugin *p;
		void *priv;
		if (loader_find_sink(n, &p, &priv) != 0) {
			LOGD(TAG, "sink %s not available", n);
			continue;
		}
		if (p->sink->start(priv, pull, NULL) == 0) {
			P.sink_p = p;
			P.sink_priv = priv;
			LOGI(TAG, "sink: %s", n);
			return 0;
		}
		LOGW(TAG, "sink %s failed to start, trying the next", n);
	}
	return -1;
}

int player_init(void)
{
	if (ring_init(&P.play, PLAYER_RING_FRAMES, CH) != 0 ||
	    ring_init(&P.tap, PLAYER_TAP_FRAMES, CH) != 0) {
		ring_free(&P.play);
		LOGE(TAG, "out of memory");
		return -1;
	}
	markq_init(&P.marks);
	P.cur_gain = 1.0f;
	P.seen_drop_seq = 0;
	atomic_store(&P.drop_seq, 0);
	player_set_volume((int)config_get_long("host.volume", 100));
	P.cur_gain = bits_to_f(atomic_load(&P.gain_bits));

	if (start_sink() != 0) {
		LOGE(TAG, "no sink could start (host.sink = %s)", config_get_str("host.sink", "pipewire,null"));
		ring_free(&P.play);
		ring_free(&P.tap);
		return -1;
	}
	if (register_verbs() != 0) LOGW(TAG, "some playback verbs could not be registered");
	pthread_mutex_lock(&P.lock);
	P.inited = 1;
	pthread_mutex_unlock(&P.lock);
	return 0;
}

void player_fini(void)
{
	pthread_mutex_lock(&P.lock);
	if (!P.inited) {
		pthread_mutex_unlock(&P.lock);
		return;
	}
	close_stream();
	P.state = PLAYER_STOPPED;
	P.inited = 0;
	pthread_mutex_unlock(&P.lock);

	ipc_unregister_owner("player");
	P.sink_p->sink->stop(P.sink_priv); /* pull is never called after this */
	P.sink_p = NULL;
	ring_free(&P.play);
	ring_free(&P.tap);
	atomic_store(&P.cur_item, 0);
	atomic_store(&P.ended_item, 0);
	atomic_store(&P.item_frames, 0);
	atomic_store(&P.underruns, 0);
	atomic_store(&P.tap_overruns, 0);
	atomic_store(&P.tap_on, 0);
}

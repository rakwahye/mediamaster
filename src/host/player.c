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
 * never has to wait for the sink. Markers carry the generation they were
 * pushed in; the sink discards older generations on a flush.
 *
 * Gapless. Three stream slots:
 *   spk  the item at the speaker
 *   dec  the item the reader is reading (ahead of spk by the ring's fill)
 *   nxt  preloaded, opened but unread
 * When dec ends, the reader marks END, moves nxt into dec, marks START at
 * the same frame and carries on writing: the sink crosses the boundary
 * without a gap. At most one boundary is in flight (dec != spk); the tick
 * retires spk once the speaker reaches dec. The reader and control share
 * the slots under `slock`; the reader never takes `lock` (control holds it
 * while joining the reader). */

#include "player.h"
#include "uri.h"
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

#define TAGS_MAX 2048

struct strm {
	const struct mm_plugin *src_p;
	void *stream;             /* NULL = empty slot */
	struct mm_stream_info info;
	char uri[1024];
	char tags[TAGS_MAX];      /* "key: value\n" lines, as status prints them */
	uint32_t item;
};

/* What a source said about a stream that is not in a slot: one being
 * opened and not placed yet (taken when it is), or one on its way out
 * (never taken). A few entries, so the second kind cannot crowd out the
 * first. */
#define PENDING 8

struct pending {
	void *stream;
	int has_info, has_tags;
	struct mm_stream_info info;
	char tags[TAGS_MAX];
};

static struct {
	pthread_mutex_t lock; /* control-side state below, up to the RT block */
	int inited;

	const struct mm_plugin *sink_p;
	void *sink_priv;

	pthread_mutex_t slock; /* spk/dec/nxt, shared with the reader */
	struct strm spk, dec, nxt;
	struct pending pend[PENDING];
	unsigned pend_next;
	uint32_t item_seq;
	uint32_t next_asked;   /* dec item we last asked the queue to follow */
	int state;
	int vol;

	pthread_t reader;
	int reader_on;
	atomic_int reader_stop;
	atomic_int reader_done;
	atomic_uint_least64_t dec_frames; /* frames of dec read so far */
	atomic_uint seeks;
	atomic_uint metas;

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
} P = { .lock = PTHREAD_MUTEX_INITIALIZER, .slock = PTHREAD_MUTEX_INITIALIZER };

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
		while (markq_peek(&P.marks, &m) && m.gen != seq) markq_pop(&P.marks);
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

static int push_mark(uint32_t kind, uint32_t item, uint64_t offset)
{
	struct mm_mark m = {
		.pos = ring_head(&P.play), .offset = offset, .item = item, .kind = kind,
		.gen = atomic_load_explicit(&P.drop_seq, memory_order_relaxed),
	};
	if (markq_push(&P.marks, &m) != 0) {
		LOGW(TAG, "marker queue full");
		return -1;
	}
	return 0;
}

/* The reader owns P.dec while it runs; it changes dec only at a boundary,
 * under slock. */
static void *reader_main(void *arg)
{
	(void)arg;
	static float buf[READ_CHUNK * CH]; /* one reader at a time */

	while (!atomic_load(&P.reader_stop)) {
		const struct mm_source_ops *src = P.dec.src_p->source;
		long n = src->read(P.dec.stream, buf, READ_CHUNK);
		if (n < 0) {
			LOGW(TAG, "%s: read error %ld", P.dec.uri, n);
			n = 0; /* treat as the end of this item */
		}
		if (n == 0) {
			if (atomic_load(&P.reader_stop)) break; /* interrupted */
			push_mark(MM_MARK_END, P.dec.item, 0);
			pthread_mutex_lock(&P.slock);
			int take = P.nxt.stream && P.spk.stream == P.dec.stream;
			if (take) {
				P.dec = P.nxt;
				memset(&P.nxt, 0, sizeof(P.nxt));
				atomic_store(&P.dec_frames, 0);
			}
			pthread_mutex_unlock(&P.slock);
			if (!take) break;
			push_mark(MM_MARK_START, P.dec.item, 0);
			LOGD(TAG, "gapless into %s", P.dec.uri);
			continue;
		}
		if (n > (long)READ_CHUNK) n = READ_CHUNK;
		uint32_t off = 0;
		while (off < (uint32_t)n && !atomic_load(&P.reader_stop)) {
			off += ring_write(&P.play, buf + (size_t)off * CH, (uint32_t)n - off);
			if (off < (uint32_t)n) sleep_ns(FULL_WAIT_NS);
		}
		atomic_fetch_add(&P.dec_frames, off);
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

/* Lock held. Interrupts a blocked read. Stop is raised before dec is read
 * under slock, so a reader crossing a boundary right now sees the stop
 * flag before its next read, and an already-switched one gets the
 * interrupt on the right stream. */
static void reader_join(void)
{
	if (!P.reader_on) return;
	atomic_store(&P.reader_stop, 1);
	pthread_mutex_lock(&P.slock);
	if (P.dec.stream && P.dec.src_p->source->interrupt) P.dec.src_p->source->interrupt(P.dec.stream);
	pthread_mutex_unlock(&P.slock);
	pthread_join(P.reader, NULL);
	P.reader_on = 0;
}

/* Lock held, reader stopped. Discard everything queued so far. */
static void flush(void)
{
	atomic_store_explicit(&P.drop_to, ring_head(&P.play), memory_order_relaxed);
	atomic_fetch_add_explicit(&P.drop_seq, 1, memory_order_release);
}

static void strm_close(struct strm *t)
{
	if (t->stream) t->src_p->source->close(t->stream);
	memset(t, 0, sizeof(*t));
}

/* Lock held. Stop the reader and close every slot. */
static void close_all(void)
{
	reader_join();
	pthread_mutex_lock(&P.slock);
	struct strm spk = P.spk, dec = P.dec, nxt = P.nxt;
	memset(&P.spk, 0, sizeof(P.spk));
	memset(&P.dec, 0, sizeof(P.dec));
	memset(&P.nxt, 0, sizeof(P.nxt));
	pthread_mutex_unlock(&P.slock);
	if (dec.stream && dec.stream != spk.stream) strm_close(&dec);
	strm_close(&spk);
	strm_close(&nxt);
	P.next_asked = 0;
}

/* ------------------------------------------------------------------ control */

/* slock held. A newly opened stream enters a slot: what its source said
 * about it before that applies now. */
static void adopt(struct strm *t)
{
	for (unsigned i = 0; i < PENDING; i++) {
		struct pending *q = &P.pend[i];
		if (!q->stream || q->stream != t->stream) continue;
		if (q->has_info) t->info = q->info;
		if (q->has_tags) memcpy(t->tags, q->tags, sizeof(t->tags));
		memset(q, 0, sizeof(*q));
	}
}

/* slock held. The entry for `stream`, made if need be. */
static struct pending *pending_for(void *stream)
{
	for (unsigned i = 0; i < PENDING; i++)
		if (P.pend[i].stream == stream) return &P.pend[i];
	struct pending *q = &P.pend[P.pend_next++ % PENDING];
	memset(q, 0, sizeof(*q));
	q->stream = stream;
	return q;
}

/* Resolve and open `uri` into t (lock held). */
static int open_strm(const char *uri, struct strm *t, char *err, size_t errlen)
{
	char scheme[32];
	void *priv;
	memset(t, 0, sizeof(*t));
	pthread_mutex_lock(&P.slock);
	memset(P.pend, 0, sizeof(P.pend)); /* only this open's stream counts */
	pthread_mutex_unlock(&P.slock);
	uri_scheme(uri, scheme, sizeof(scheme));
	if (loader_find_source(scheme, &t->src_p, &priv) != 0) {
		snprintf(err, errlen, "no source for '%s:'", scheme);
		return -1;
	}
	if (t->src_p->source->open(priv, uri, &t->stream, &t->info) != 0 || !t->stream) {
		snprintf(err, errlen, "%s could not open %s", t->src_p->name, uri);
		memset(t, 0, sizeof(*t));
		return -1;
	}
	snprintf(t->uri, sizeof(t->uri), "%s", uri);
	if (++P.item_seq == 0) P.item_seq = 1;
	t->item = P.item_seq;
	return 0;
}

int player_play(const char *uri, char *err, size_t errlen)
{
	struct strm t;
	pthread_mutex_lock(&P.lock);
	if (!P.inited) {
		pthread_mutex_unlock(&P.lock);
		snprintf(err, errlen, "no sink");
		return -1;
	}
	/* Open before stopping what plays, so a bad URI leaves it alone. */
	if (open_strm(uri, &t, err, errlen) != 0) {
		pthread_mutex_unlock(&P.lock);
		return -1;
	}
	close_all();
	flush();
	pthread_mutex_lock(&P.slock);
	adopt(&t);
	P.spk = P.dec = t;
	pthread_mutex_unlock(&P.slock);
	atomic_store(&P.dec_frames, 0);
	push_mark(MM_MARK_START, t.item, 0);
	atomic_store(&P.paused, 0);
	int rc = reader_start();
	if (rc != 0) {
		close_all();
		snprintf(err, errlen, "could not start reader");
	}
	P.state = rc ? PLAYER_STOPPED : PLAYER_PLAYING;
	pthread_mutex_unlock(&P.lock);
	if (!rc) LOGI(TAG, "playing %s (%s)", uri, t.src_p->name);
	return rc;
}

int player_preload(const char *uri, char *err, size_t errlen)
{
	struct strm t, old;
	pthread_mutex_lock(&P.lock);
	if (P.state == PLAYER_STOPPED) {
		pthread_mutex_unlock(&P.lock);
		snprintf(err, errlen, "nothing playing");
		return -1;
	}
	if (open_strm(uri, &t, err, errlen) != 0) {
		pthread_mutex_unlock(&P.lock);
		return -1;
	}
	pthread_mutex_lock(&P.slock);
	old = P.nxt;
	adopt(&t);
	P.nxt = t;
	pthread_mutex_unlock(&P.slock);
	strm_close(&old);
	pthread_mutex_unlock(&P.lock);
	LOGD(TAG, "preloaded %s", uri);
	return 0;
}

void player_drop_preload(void)
{
	pthread_mutex_lock(&P.lock);
	pthread_mutex_lock(&P.slock);
	struct strm old = P.nxt;
	memset(&P.nxt, 0, sizeof(P.nxt));
	pthread_mutex_unlock(&P.slock);
	strm_close(&old);
	P.next_asked = 0;
	pthread_mutex_unlock(&P.lock);
}

int player_preload_state(void)
{
	pthread_mutex_lock(&P.slock);
	int st = P.spk.stream && P.dec.stream != P.spk.stream ? 2 : P.nxt.stream ? 1 : 0;
	pthread_mutex_unlock(&P.slock);
	return st;
}

int player_wants_next(void)
{
	int want = 0;
	pthread_mutex_lock(&P.lock);
	pthread_mutex_lock(&P.slock);
	if (P.state != PLAYER_STOPPED && P.dec.stream && P.dec.stream == P.spk.stream &&
	    !P.nxt.stream && P.next_asked != P.dec.item) {
		uint64_t dur = P.dec.info.duration_ms;
		uint64_t at = atomic_load(&P.dec_frames) * 1000u / MM_PCM_RATE;
		uint64_t window = (uint64_t)config_get_long("host.preload_s", 10) * 1000u;
		int ended = P.reader_on && atomic_load(&P.reader_done);
		if (ended || (dur && (at >= dur || dur - at <= window))) {
			want = 1;
			P.next_asked = P.dec.item;
		}
	}
	pthread_mutex_unlock(&P.slock);
	pthread_mutex_unlock(&P.lock);
	return want;
}

void player_stop(void)
{
	pthread_mutex_lock(&P.lock);
	close_all();
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

/* Seek always applies to the item at the speaker. If the reader is already
 * into the next item, that read-ahead is discarded and the queue will be
 * asked for the next item again. */
int player_seek_ms(int64_t ms, int relative, char *err, size_t errlen)
{
	pthread_mutex_lock(&P.lock);
	int rc = -1;
	if (P.state == PLAYER_STOPPED || !P.spk.stream) {
		snprintf(err, errlen, "nothing playing");
		goto out;
	}
	if (!(P.spk.info.flags & MM_STREAM_SEEKABLE) || !P.spk.src_p->source->seek) {
		snprintf(err, errlen, "not seekable");
		goto out;
	}
	int64_t target = relative ? (int64_t)player_position_ms() + ms : ms;
	if (target < 0) target = 0;
	if (P.spk.info.duration_ms && (uint64_t)target > P.spk.info.duration_ms)
		target = (int64_t)P.spk.info.duration_ms;

	reader_join();
	pthread_mutex_lock(&P.slock);
	struct strm ahead = { 0 };
	if (P.dec.stream != P.spk.stream) {
		ahead = P.dec;
		P.dec = P.spk;
		P.next_asked = 0;
	}
	pthread_mutex_unlock(&P.slock);
	strm_close(&ahead);

	uint64_t from_frames;
	if (P.spk.src_p->source->seek(P.spk.stream, (uint64_t)target) != 0) {
		snprintf(err, errlen, "seek failed");
		from_frames = atomic_load(&P.item_frames); /* carry on from wherever it is */
	} else {
		from_frames = (uint64_t)target * MM_PCM_RATE / 1000u;
		rc = 0;
		atomic_fetch_add(&P.seeks, 1);
	}
	flush();
	atomic_store(&P.dec_frames, from_frames);
	/* Report the target at once; until the sink applies the flush (one
	 * callback) it would still count the old position. The START marker
	 * then sets it exactly. */
	atomic_store(&P.item_frames, from_frames);
	push_mark(MM_MARK_START, P.spk.item, from_frames);
	if (reader_start() != 0) {
		snprintf(err, errlen, "could not restart reader");
		rc = -1;
	}
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

int player_tick(void)
{
	int ev = 0;
	pthread_mutex_lock(&P.lock);

	/* The speaker reached the item the reader moved on to. */
	unsigned at_spk = atomic_load_explicit(&P.cur_item, memory_order_relaxed);
	pthread_mutex_lock(&P.slock);
	struct strm retired = { 0 };
	if (P.spk.stream && P.dec.stream != P.spk.stream && at_spk == P.dec.item) {
		retired = P.spk;
		P.spk = P.dec;
		ev |= PLAYER_EV_ADVANCED;
	}
	pthread_mutex_unlock(&P.slock);
	if (ev & PLAYER_EV_ADVANCED) {
		LOGI(TAG, "now playing %s", P.spk.uri);
		strm_close(&retired);
	}

	if (P.reader_on && atomic_load(&P.reader_done) && !atomic_load(&P.reader_stop)) {
		if (P.nxt.stream && P.spk.stream == P.dec.stream) {
			/* The reader ran out before the next item was ready (or while a
			 * boundary was still in flight): continue from here. */
			reader_join();
			pthread_mutex_lock(&P.slock);
			P.dec = P.nxt;
			memset(&P.nxt, 0, sizeof(P.nxt));
			pthread_mutex_unlock(&P.slock);
			atomic_store(&P.dec_frames, 0);
			push_mark(MM_MARK_START, P.dec.item, 0);
			reader_start();
		} else if (!P.nxt.stream && P.spk.stream == P.dec.stream &&
		           atomic_load_explicit(&P.ended_item, memory_order_acquire) == P.dec.item) {
			LOGI(TAG, "finished %s", P.spk.uri);
			close_all();
			atomic_store(&P.cur_item, 0);
			P.state = PLAYER_STOPPED;
			ev |= PLAYER_EV_FINISHED;
		}
	}
	pthread_mutex_unlock(&P.lock);
	return ev;
}

uint32_t player_item(void)
{
	pthread_mutex_lock(&P.slock);
	uint32_t it = P.spk.stream ? P.spk.item : 0;
	pthread_mutex_unlock(&P.slock);
	return it;
}

uint32_t player_seek_count(void) { return atomic_load(&P.seeks); }
uint32_t player_meta_count(void) { return atomic_load(&P.metas); }

void player_stream_update(void *stream, const struct mm_stream_info *info)
{
	if (!stream || !info) return;
	pthread_mutex_lock(&P.slock);
	int spk = 0, placed = 0;
	if (P.spk.stream == stream) { P.spk.info = *info; spk = placed = 1; }
	if (P.dec.stream == stream) { P.dec.info = *info; placed = 1; }
	if (P.nxt.stream == stream) { P.nxt.info = *info; placed = 1; }
	if (!placed) {
		struct pending *q = pending_for(stream);
		q->info = *info;
		q->has_info = 1;
	}
	pthread_mutex_unlock(&P.slock);
	if (spk) atomic_fetch_add(&P.metas, 1);
}

static const char *const tag_keys[] = {
	"title", "artist", "album", "albumartist", "tracknumber", "discnumber", "date", "genre", "art", NULL
};

/* kv → "key: value\n" lines, known keys only, one line per value. */
static void tags_format(char *out, size_t cap, const char *const *kv)
{
	size_t o = 0;
	out[0] = '\0';
	for (; kv && kv[0] && kv[1]; kv += 2) {
		int known = 0;
		for (const char *const *k = tag_keys; *k && !known; k++) known = !strcmp(*k, kv[0]);
		if (!known || !kv[1][0]) continue;
		size_t need = strlen(kv[0]) + 2 + strlen(kv[1]) + 1;
		if (o + need + 1 > cap) break; /* whole lines only */
		o += (size_t)sprintf(out + o, "%s: ", kv[0]);
		for (const char *v = kv[1]; *v; v++) out[o++] = (*v == '\n' || *v == '\r') ? ' ' : *v;
		out[o++] = '\n';
		out[o] = '\0';
	}
}

void player_stream_tags(void *stream, const char *const *kv)
{
	char tags[TAGS_MAX];
	if (!stream) return;
	tags_format(tags, sizeof(tags), kv);
	pthread_mutex_lock(&P.slock);
	int spk = 0, placed = 0;
	if (P.spk.stream == stream) { memcpy(P.spk.tags, tags, sizeof(tags)); spk = placed = 1; }
	if (P.dec.stream == stream) { memcpy(P.dec.tags, tags, sizeof(tags)); placed = 1; }
	if (P.nxt.stream == stream) { memcpy(P.nxt.tags, tags, sizeof(tags)); placed = 1; }
	if (!placed) {
		struct pending *q = pending_for(stream);
		memcpy(q->tags, tags, sizeof(tags));
		q->has_tags = 1;
	}
	pthread_mutex_unlock(&P.slock);
	if (spk) atomic_fetch_add(&P.metas, 1);
}

void player_current_uri(char *buf, size_t cap)
{
	pthread_mutex_lock(&P.slock);
	snprintf(buf, cap, "%s", P.spk.uri);
	pthread_mutex_unlock(&P.slock);
}

uint32_t player_buffered_frames(void) { return ring_fill(&P.play); }
uint64_t player_underruns(void) { return atomic_load(&P.underruns); }

void player_tap_enable(int on) { atomic_store(&P.tap_on, on); }
uint32_t player_tap_read(float *dst, uint32_t frames) { return ring_read(&P.tap, dst, frames); }
uint64_t player_tap_overruns(void) { return atomic_load(&P.tap_overruns); }

/* ------------------------------------------------------------------ verbs */

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
	pthread_mutex_lock(&P.slock);
	const struct strm *k = &P.spk;
	const char *next = P.dec.stream != P.spk.stream ? P.dec.uri : P.nxt.stream ? P.nxt.uri : "-";
	uint64_t pos = player_position_ms();
	char dur[32] = "-";
	if (k->info.duration_ms)
		snprintf(dur, sizeof(dur), "%.3f", (double)k->info.duration_ms / 1000.0);
	snprintf(c->reply, c->reply_len,
	         "state: %s\nitem: %u\nuri: %s\nsource: %s\n%sposition: %.3f\nduration: %s\nseekable: %s\n"
	         "next: %s\nvolume: %d\nsink: %s\nbuffered: %.3f\nunderruns: %" PRIu64 "\n",
	         names[P.state], k->stream ? k->item : 0u, k->uri[0] ? k->uri : "-", k->src_p ? k->src_p->name : "-",
	         k->tags, (double)pos / 1000.0, dur,
	         (k->info.flags & MM_STREAM_SEEKABLE) ? "yes" : "no", next,
	         P.vol, P.sink_p ? P.sink_p->name : "-",
	         (double)ring_fill(&P.play) / MM_PCM_RATE,
	         (uint64_t)atomic_load(&P.underruns));
	pthread_mutex_unlock(&P.slock);
	pthread_mutex_unlock(&P.lock);
	return MM_REPLY_RAW;
}

static int register_verbs(void)
{
	int rc = 0;
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
	close_all();
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

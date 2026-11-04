/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The player against the real raw source and a test sink whose pull is
 * driven by hand from this thread, so every check is deterministic. */
#include "player.h"
#include "config.h"
#include "ipc.h"
#include "loader.h"
#include "check.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const struct mm_plugin *mm_static_entry_raw(void);

/* Gated source: hands out only the frames the test has allowed, blocking
 * otherwise. Lets the test starve the sink on purpose. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static long g_allow;
static int g_intr;

static void gate_allow(long frames)
{
	pthread_mutex_lock(&g_mu);
	g_allow += frames;
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
}

static int gate_open(void *priv, const char *uri, void **st, struct mm_stream_info *in)
{
	(void)priv; (void)uri;
	pthread_mutex_lock(&g_mu);
	g_allow = 0;
	g_intr = 0;
	pthread_mutex_unlock(&g_mu);
	in->duration_ms = 0;
	in->flags = MM_STREAM_LIVE;
	*st = &g_allow;
	return 0;
}
static long gate_read(void *st, float *dst, uint32_t frames)
{
	(void)st;
	pthread_mutex_lock(&g_mu);
	while (!g_allow && !g_intr) pthread_cond_wait(&g_cv, &g_mu);
	long n = 0;
	if (!g_intr) {
		n = g_allow < (long)frames ? g_allow : (long)frames;
		g_allow -= n;
	}
	g_intr = 0;
	pthread_mutex_unlock(&g_mu);
	for (long i = 0; i < n * (long)MM_PCM_CHANNELS; i++) dst[i] = 0.5f;
	return n;
}
static void gate_interrupt(void *st)
{
	(void)st;
	pthread_mutex_lock(&g_mu);
	g_intr = 1;
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
}
static void gate_close(void *st) { (void)st; }
static const struct mm_source_ops gate_ops = { .open = gate_open, .read = gate_read, .interrupt = gate_interrupt, .close = gate_close };
static int gate_init(const struct mm_host *h, void **priv) { (void)h; *priv = NULL; return 0; }
static const struct mm_plugin gatesrc = { .abi = MM_ABI, .name = "gate", .schemes = "gate", .init = gate_init, .source = &gate_ops };

static mm_pull_fn s_pull;
static void *s_ctx;
static int s_started, s_stopped;

static int ts_start(void *priv, mm_pull_fn pull, void *ctx) { (void)priv; s_pull = pull; s_ctx = ctx; s_started++; return 0; }
static void ts_stop(void *priv) { (void)priv; s_stopped++; }
static int ts_init(const struct mm_host *h, void **priv) { (void)h; *priv = NULL; return 0; }
static int bad_start(void *priv, mm_pull_fn pull, void *ctx) { (void)priv; (void)pull; (void)ctx; return -1; }

static const struct mm_sink_ops ts_ops = { .start = ts_start, .stop = ts_stop };
static const struct mm_sink_ops bad_ops = { .start = bad_start, .stop = ts_stop };
static const struct mm_plugin testsink = { .abi = MM_ABI, .name = "testsink", .init = ts_init, .sink = &ts_ops };
static const struct mm_plugin badsink = { .abi = MM_ABI, .name = "badsink", .init = ts_init, .sink = &bad_ops };

#define CH MM_PCM_CHANNELS
#define AMP 0.2
#define HZ 1000.0
static const double W = 2.0 * 3.14159265358979323846 * HZ / MM_PCM_RATE;

static float s_buf[CH * 48000];

static void pull(uint32_t frames) { s_pull(s_ctx, s_buf, frames); }

static float peak(uint32_t frames)
{
	float p = 0;
	for (uint32_t i = 0; i < frames * CH; i++) if (fabsf(s_buf[i]) > p) p = fabsf(s_buf[i]);
	return p;
}

/* Wait (bounded) for the reader to queue at least `frames`. */
static int wait_buffered(uint32_t frames)
{
	for (int i = 0; i < 2000; i++) {
		if (player_buffered_frames() >= frames) return 0;
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	return -1;
}

/* Keep the sink running until the player retires the stream (the reader
 * posts END asynchronously). @returns 0 once stopped. */
static int run_to_stop(void)
{
	for (int i = 0; i < 2000; i++) {
		pull(480);
		player_tick();
		if (player_state() == PLAYER_STOPPED) return 0;
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	return -1;
}

static int near(float a, double b) { return fabs((double)a - b) < 1e-4; }

static int finite_stream(void)
{
	char err[128];
	CHECK(player_play("nosuch:thing", err, sizeof(err)) != 0 && strstr(err, "no source"), "an unclaimed scheme is refused with a reason");
	CHECK(player_play("sine:-5", err, sizeof(err)) != 0, "a URI the source rejects is refused");
	CHECK(player_state() == PLAYER_STOPPED, "refusals leave the player stopped");

	CHECK(player_play("sine:1000:0.5", err, sizeof(err)) == 0, "a finite sine plays");
	CHECK(player_state() == PLAYER_PLAYING, "state is playing");
	CHECK(wait_buffered(24000) == 0, "the reader queues the whole half second");

	pull(480);
	CHECK(near(s_buf[0], 0.0) && near(s_buf[2], AMP * sin(W)) && near(s_buf[3], s_buf[2]), "the first frames are the sine from phase 0, both channels");
	CHECK(player_position_ms() == 10, "position counts frames at the speaker");
	pull(24000 - 480);
	CHECK(near(s_buf[(24000 - 481) * CH], AMP * sin(W * 23999)), "the last frame is the sine's last sample");
	CHECK(run_to_stop() == 0, "the tick retires a finished stream");
	CHECK(peak(480) == 0.0f, "after the end the sink gets silence");
	CHECK(player_underruns() == 0, "silence after a finished stream is not an underrun");
	return 0;
}

static int pause_volume_tap(void)
{
	char err[128];
	CHECK(player_play("sine:1000", err, sizeof(err)) == 0, "an endless sine plays");
	CHECK(wait_buffered(4800) == 0, "audio is queued");
	pull(480);
	uint64_t pos = player_position_ms();

	CHECK(player_pause(1) == 1 && player_state() == PLAYER_PAUSED, "pause");
	pull(480);
	CHECK(peak(480) == 0.0f && player_position_ms() == pos, "paused: silence and the position holds");
	CHECK(player_pause(-1) == 0 && player_state() == PLAYER_PLAYING, "toggle resumes");
	pull(480);
	CHECK(near(s_buf[0], AMP * sin(W * 480)), "resume continues exactly where it paused");

	player_set_volume(50);
	pull(480);           /* ramp buffer */
	pull(480);
	CHECK(fabsf(peak(480) - (float)(AMP * 0.125)) < 2e-3f, "volume 50 applies a 0.125 gain after one ramp buffer");

	player_tap_enable(1);
	player_set_volume(0);
	pull(480);
	pull(480);
	CHECK(peak(480) == 0.0f, "volume 0 is silent at the sink");
	static float tap[CH * 2048];
	uint32_t n = player_tap_read(tap, 2048);
	CHECK(n == 960, "the tap holds exactly what the sink played since it was enabled");
	float tp = 0;
	for (uint32_t i = 0; i < n * CH; i++) if (fabsf(tap[i]) > tp) tp = fabsf(tap[i]);
	CHECK(fabsf(tp - (float)AMP) < 2e-3f, "the tap is taken before volume");
	player_tap_enable(0);
	player_set_volume(100);

	player_stop();
	CHECK(player_state() == PLAYER_STOPPED, "stop");
	pull(480);
	CHECK(peak(480) == 0.0f && player_buffered_frames() == 0, "stop discards everything queued");
	CHECK(player_underruns() == 0, "a stopped player does not count underruns");
	CHECK(player_pause(1) == 0, "pause while stopped is a no-op");
	return 0;
}

static int seeking(void)
{
	char err[128];
	CHECK(player_seek_ms(1000, 0, err, sizeof(err)) != 0, "seek with nothing playing is refused");
	CHECK(player_play("sine:1000", err, sizeof(err)) == 0, "endless sine");
	CHECK(player_seek_ms(1000, 0, err, sizeof(err)) != 0 && strstr(err, "not seekable"), "a live stream refuses seek");
	player_stop();

	CHECK(player_play("sine:1000:10", err, sizeof(err)) == 0, "a ten second sine");
	CHECK(wait_buffered(9600) == 0, "audio is queued");
	pull(480);
	CHECK(player_seek_ms(5000, 0, err, sizeof(err)) == 0, "absolute seek");
	pull(0); /* the sink applies the flush on its next callback */
	CHECK(player_buffered_frames() < 9600, "the next callback discards pre-seek audio");
	CHECK(wait_buffered(480) == 0, "audio after the seek is queued");
	pull(480);
	CHECK(near(s_buf[0], AMP * sin(W * 240000)), "the first frame after seeking to 5 s is the sample at 5 s");
	CHECK(player_position_ms() == 5010, "position restarts from the seek target");

	CHECK(player_seek_ms(-2000, 1, err, sizeof(err)) == 0, "relative seek backwards");
	pull(0);
	CHECK(wait_buffered(480) == 0, "queued");
	pull(480);
	CHECK(player_position_ms() == 3020, "relative seek moves from the current position");

	CHECK(player_seek_ms(60000, 0, err, sizeof(err)) == 0, "seek past the end clamps");
	CHECK(run_to_stop() == 0, "seeking to the end finishes the stream");
	return 0;
}

static int replace_and_files(void)
{
	char err[128], path[64];
	CHECK(player_play("sine:1000:10", err, sizeof(err)) == 0, "first stream");
	CHECK(wait_buffered(4800) == 0, "queued");
	pull(480);

	snprintf(path, sizeof(path), "/tmp/mm-player-test.%d.f32", (int)getpid());
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL, "fixture file opens");
	static float ramp[CH * 3000];
	for (unsigned i = 0; i < CH * 3000; i++) ramp[i] = (float)i / (CH * 3000);
	fwrite(ramp, sizeof(float), CH * 3000, f);
	fclose(f);

	char uri[80];
	snprintf(uri, sizeof(uri), "raw:%s", path);
	CHECK(player_play(uri, err, sizeof(err)) == 0, "play replaces the current stream");
	pull(0);
	CHECK(wait_buffered(3000) == 0, "the file is queued");
	pull(3000);
	CHECK(!memcmp(s_buf, ramp, sizeof(ramp)), "a raw file plays bit-exact, with nothing of the old stream before it");
	CHECK(player_position_ms() == 62, "position belongs to the new item");
	CHECK(run_to_stop() == 0, "the file finishes");
	unlink(path);
	return 0;
}

static int underruns(void)
{
	char err[128];
	uint64_t base = player_underruns();
	CHECK(player_play("gate:", err, sizeof(err)) == 0, "the gated source plays");
	pull(480);
	pull(480);
	CHECK(player_underruns() == base, "silence before the first audio arrives is not an underrun");

	gate_allow(960);
	CHECK(wait_buffered(960) == 0, "the allowed audio is queued");
	pull(480);
	CHECK(player_underruns() == base, "audio flowing: no underrun");
	pull(960);
	CHECK(player_underruns() == base + 1, "running dry mid-stream counts one underrun");
	CHECK(fabsf(s_buf[0] - 0.5f) < 1e-6f && s_buf[480 * CH] == 0.0f, "the sink gets the audio there was, then silence");

	player_stop();
	pull(480);
	CHECK(player_underruns() == base + 1, "silence after stop is not an underrun");
	return 0;
}

int main(void)
{
	config_override("host.sink=badsink, testsink");
	config_load(NULL);
	loader_add(&badsink, NULL, "test");
	loader_add(&testsink, NULL, "test");
	loader_add(mm_static_entry_raw(), NULL, "builtin");
	loader_add(&gatesrc, NULL, "test");
	loader_init_all();

	CHECK(player_init() == 0, "the player starts");
	CHECK(s_started == 1 && s_pull, "a failing sink is skipped for the next in host.sink");
	CHECK(ipc_has_verb("play") && ipc_has_verb("status"), "playback verbs are registered");

	RUN(finite_stream);
	RUN(pause_volume_tap);
	RUN(seeking);
	RUN(replace_and_files);
	RUN(underruns);

	player_fini();
	CHECK(s_stopped == 1, "fini stops the sink");
	CHECK(!ipc_has_verb("play"), "fini drops the playback verbs");
	loader_fini_all();
	config_free();
	CHECK_PASS("test-player");
}

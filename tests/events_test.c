/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host services for control plugins: a test plugin drives the player
 * through host->command and records what host->subscribe delivers. */
#include "events.h"
#include "player.h"
#include "queue.h"
#include "config.h"
#include "ipc.h"
#include "loader.h"
#include "check.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

const struct mm_plugin *mm_static_entry_raw(void);

static mm_pull_fn s_pull;
static void *s_ctx;
static int ts_start(void *priv, mm_pull_fn pull, void *ctx) { (void)priv; s_pull = pull; s_ctx = ctx; return 0; }
static void ts_stop(void *priv) { (void)priv; }
static int ts_init(const struct mm_host *h, void **priv) { (void)h; *priv = NULL; return 0; }
static const struct mm_sink_ops ts_ops = { .start = ts_start, .stop = ts_stop };
static const struct mm_plugin testsink = { .abi = MM_ABI, .name = "testsink", .init = ts_init, .sink = &ts_ops };

/* A source whose streams learn their length after open: silence, as long
 * as the test likes, until it reports a duration. */
struct late { int id; };
static struct late g_late[4];
static int g_nlate;
static int late_open(void *priv, const char *uri, void **st, struct mm_stream_info *in)
{
	(void)priv; (void)uri;
	if (g_nlate == 4) return -1;
	*st = &g_late[g_nlate++];
	in->duration_ms = 0;
	in->flags = MM_STREAM_LIVE;
	return 0;
}
static long late_read(void *st, float *dst, uint32_t frames)
{
	(void)st;
	memset(dst, 0, (size_t)frames * MM_PCM_CHANNELS * sizeof(float));
	return (long)frames;
}
static int late_seek(void *st, uint64_t ms) { (void)st; (void)ms; return 0; }
static void late_close(void *st) { (void)st; }
static int late_init(const struct mm_host *h, void **priv) { (void)h; *priv = NULL; return 0; }
static const struct mm_source_ops late_ops = { .open = late_open, .read = late_read, .seek = late_seek, .close = late_close };
static const struct mm_plugin latesrc = { .abi = MM_ABI, .name = "late", .schemes = "late", .init = late_init, .source = &late_ops };

/* The control plugin under test. */
static const struct mm_host *H;
static uint32_t g_seen;
static int g_calls;
static void on_event(void *priv, uint32_t ev) { (void)priv; g_seen |= ev; g_calls++; }
static int ctl_init(const struct mm_host *h, void **priv)
{
	H = h;
	*priv = NULL;
	if (h->size < sizeof(struct mm_host) || !h->command || !h->subscribe) return -1;
	return h->subscribe(h, MM_EV_STATE | MM_EV_TRACK | MM_EV_VOLUME | MM_EV_SEEK | MM_EV_QUEUE | MM_EV_META,
	                    on_event, NULL);
}
static const struct mm_plugin ctl = { .abi = MM_ABI, .name = "ctl", .init = ctl_init };

static char R[4096];
static int cmd(const char *a0, const char *a1, const char *a2)
{
	const char *argv[4] = { a0, a1, a2, NULL };
	int argc = a2 ? 3 : a1 ? 2 : 1;
	return H->command(H, argc, argv, R, sizeof(R));
}

static float s_buf[MM_PCM_CHANNELS * 480];

/* One main-loop iteration. */
static uint32_t step(void)
{
	s_pull(s_ctx, s_buf, 480);
	queue_tick();
	g_seen = 0;
	events_poll();
	return g_seen;
}

static uint32_t step_until(uint32_t want)
{
	uint32_t all = 0;
	for (int i = 0; i < 2000 && (all & want) != want; i++) {
		all |= step();
		struct timespec ts = { 0, 500000 };
		nanosleep(&ts, NULL);
	}
	return all;
}

static int commands(void)
{
	CHECK(cmd("vol", "40", NULL) == MM_OK && !strcmp(R, "40"), "a command runs in-process and returns the bare reply");
	CHECK(cmd("nosuch", NULL, NULL) == MM_ERR && strstr(R, "unknown verb"), "an unknown verb is MM_ERR with a reason");
	CHECK(cmd("status", NULL, NULL) == MM_REPLY_RAW && strstr(R, "state: stopped\n"), "a raw verb returns its raw text");
	CHECK(cmd("play", "sine:-1", NULL) == MM_ERR, "errors come back as MM_ERR");
	return 0;
}

static int events(void)
{
	step();
	CHECK(step() == 0, "nothing happens, nothing is sent");

	CHECK(cmd("queue", "add", "sine:300:0.05") == MM_OK, "queue add");
	CHECK(step() == MM_EV_QUEUE, "a queue edit is a queue event");

	CHECK(cmd("play", NULL, NULL) == MM_OK, "play");
	uint32_t ev = step();
	CHECK((ev & (MM_EV_STATE | MM_EV_TRACK | MM_EV_QUEUE)) == (MM_EV_STATE | MM_EV_TRACK | MM_EV_QUEUE),
	      "starting playback: state, track and queue (cursor) events");

	cmd("vol", "70", NULL);
	CHECK(step() == MM_EV_VOLUME, "volume");
	cmd("pause", NULL, NULL);
	CHECK(step() == MM_EV_STATE, "pause");
	cmd("resume", NULL, NULL);
	CHECK(step() == MM_EV_STATE, "resume");
	cmd("stop", NULL, NULL);

	cmd("play", "sine:440:5", NULL);
	step();
	cmd("seek", "2", NULL);
	CHECK(step() & MM_EV_SEEK, "seek");
	cmd("seek", "+1", NULL);
	CHECK(step() & MM_EV_SEEK, "every seek, also relative ones");

	/* A gapless advance is a track change with no state change. */
	cmd("queue", "clear", NULL);
	cmd("queue", "add", "sine:100:0.05");
	cmd("queue", "add", "sine:200:0.05");
	cmd("queue", "play", "1");
	step();
	ev = step_until(MM_EV_TRACK);
	CHECK(ev & MM_EV_TRACK, "the speaker crossing into the next item is a track event");
	CHECK(!(ev & MM_EV_STATE), "and not a state change");
	ev = step_until(MM_EV_STATE);
	CHECK(ev & MM_EV_STATE && player_state() == PLAYER_STOPPED, "the end of the queue is a state event");
	return 0;
}

static int field_is(const char *key, const char *want)
{
	char line[256];
	snprintf(line, sizeof(line), "\n%s: %s\n", key, want);
	cmd("status", NULL, NULL);
	char text[sizeof(R) + 2] = "\n";
	strcat(text, R);
	return strstr(text, line) != NULL;
}

static int late_info(void)
{
	struct mm_host old = *H;
	old.size = (uint32_t)offsetof(struct mm_host, stream_update);
	CHECK(MM_HOST_HAS(H, stream_update), "this host has stream_update");
	CHECK(!MM_HOST_HAS(&old, stream_update), "a host built before it reads as not having it");

	g_nlate = 0;
	CHECK(cmd("play", "late:a", NULL) == MM_OK, "play a stream of unknown length");
	step();
	CHECK(field_is("duration", "-") && field_is("seekable", "no"), "it starts unknown and unseekable");
	CHECK(cmd("seek", "5", NULL) == MM_ERR, "seek is refused while unknown");

	struct mm_stream_info known = { 90000, MM_STREAM_SEEKABLE };
	H->stream_update(H, &g_late[0], &known);
	uint32_t ev = step();
	CHECK(ev & MM_EV_META, "an update to the playing item is a meta event");
	CHECK(field_is("duration", "90.000") && field_is("seekable", "yes"), "status shows the reported length");
	CHECK(cmd("seek", "5", NULL) == MM_OK, "and seek now works");
	step();

	/* Preload a second stream, update it while it waits: no event yet. */
	H->stream_update(H, &g_late[3], &known); /* not open: ignored */
	CHECK(!(step() & MM_EV_META), "an update for a stream that is not open does nothing");
	CHECK(player_preload("late:b", R, sizeof(R)) == 0, "preload");
	struct mm_stream_info other = { 30000, MM_STREAM_SEEKABLE };
	H->stream_update(H, &g_late[1], &other);
	CHECK(!(step() & MM_EV_META), "an update to a preloaded item is not a meta event");
	CHECK(field_is("duration", "90.000"), "and does not touch the playing item");
	cmd("stop", NULL, NULL);
	step();
	return 0;
}

int main(void)
{
	config_override("host.sink=testsink");
	config_load(NULL);
	loader_add(&testsink, NULL, "test");
	loader_add(mm_static_entry_raw(), NULL, "builtin");
	loader_add(&ctl, NULL, "test");
	loader_add(&latesrc, NULL, "test");
	loader_init_all();
	CHECK(loader_state("ctl") == MM_PLUGIN_ACTIVE, "the control plugin got command and subscribe");
	CHECK(player_init() == 0 && queue_init() == 0, "player and queue");

	RUN(commands);
	RUN(events);
	RUN(late_info);

	queue_fini();
	player_fini();
	loader_fini_all();
	int before = g_calls;
	events_poll();
	events_poll();
	CHECK(g_calls == before, "no callbacks after the plugin is gone");
	config_free();
	CHECK_PASS("test-events");
}

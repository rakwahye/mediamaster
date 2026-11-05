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
	return h->subscribe(h, MM_EV_STATE | MM_EV_TRACK | MM_EV_VOLUME | MM_EV_SEEK | MM_EV_QUEUE, on_event, NULL);
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

int main(void)
{
	config_override("host.sink=testsink");
	config_load(NULL);
	loader_add(&testsink, NULL, "test");
	loader_add(mm_static_entry_raw(), NULL, "builtin");
	loader_add(&ctl, NULL, "test");
	loader_init_all();
	CHECK(loader_state("ctl") == MM_PLUGIN_ACTIVE, "the control plugin got command and subscribe");
	CHECK(player_init() == 0 && queue_init() == 0, "player and queue");

	RUN(commands);
	RUN(events);

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

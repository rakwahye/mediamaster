/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The queue driving the real player, raw source and a hand-driven sink.
 * Items are short tones whose frequency names them, so "what is playing"
 * is read from the player's current URI. */
#include "queue.h"
#include "catalog.h"
#include "player.h"
#include "config.h"
#include "ipc.h"
#include "loader.h"
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
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

static float s_buf[MM_PCM_CHANNELS * 4800];

/* A catalog for "box:" URIs whose items are raw tones:
 *   box:            top: box:a, box:empty
 *   box:a           sine:300, list box:b, sine:400, list box:bad, list box:loop
 *   box:b           sine:350
 *   box:bad         fails
 *   box:loop        contains itself
 *   box:empty       nothing
 *   box:t           a track (not a list)
 *   box:dud         a list whose only track no source plays */
static int g_lists; /* list calls, to see expansion stop */
static uint32_t g_flags; /* flags of the last list call */
static int emit_list(mm_item_fn emit, void *ctx, uint32_t kind, const char *uri, const char *title)
{
	struct mm_item it = { kind, uri, title, "Artist\tTab", NULL, kind == MM_ITEM_TRACK ? 61000 : 0 };
	return emit(ctx, &it);
}
static int box_list(void *priv, const char *uri, uint32_t flags, mm_item_fn emit, void *ctx,
                    char *err, size_t errlen)
{
	(void)priv;
	g_lists++;
	g_flags = flags;
	if (!strcmp(uri, "box:")) {
		emit_list(emit, ctx, MM_ITEM_LIST, "box:a", "A");
		emit_list(emit, ctx, MM_ITEM_LIST, "box:empty", "Empty");
	} else if (!strcmp(uri, "box:a")) {
		if (emit_list(emit, ctx, MM_ITEM_TRACK, "sine:300:5", "Three")) return 0;
		if (emit_list(emit, ctx, MM_ITEM_LIST, "box:b", "B")) return 0;
		if (emit_list(emit, ctx, MM_ITEM_TRACK, "sine:400:5", "Four")) return 0;
		if (emit_list(emit, ctx, MM_ITEM_LIST, "box:bad", "Bad")) return 0;
		emit_list(emit, ctx, MM_ITEM_LIST, "box:loop", "Loop");
	} else if (!strcmp(uri, "box:b")) {
		emit_list(emit, ctx, MM_ITEM_TRACK, "sine:350:5", "Three-fifty");
	} else if (!strcmp(uri, "box:loop")) {
		emit_list(emit, ctx, MM_ITEM_LIST, "box:loop", "Loop");
	} else if (!strcmp(uri, "box:dud")) {
		emit_list(emit, ctx, MM_ITEM_TRACK, "nosuch:x", "Dud");
	} else if (!strcmp(uri, "box:t")) {
		return MM_NOT_A_LIST;
	} else if (strcmp(uri, "box:empty") != 0) {
		snprintf(err, errlen, "no such box: %s", uri);
		return -1;
	}
	return 0;
}
static int box_init(const struct mm_host *h, void **priv) { (void)h; *priv = NULL; return 0; }
static const struct mm_catalog_ops box_ops = { .list = box_list };
static const struct mm_plugin boxcat = { .abi = MM_ABI, .name = "box", .schemes = "box", .init = box_init, .catalog = &box_ops };

static char R[1 << 16];
static int run(const char *a0, const char *a1, const char *a2)
{
	char *argv[4] = { (char *)a0, (char *)a1, (char *)a2, NULL };
	int argc = a2 ? 3 : a1 ? 2 : 1;
	return ipc_dispatch(argc, argv, NULL, 0, R, sizeof(R));
}

static const char *now(void)
{
	static char u[256];
	player_current_uri(u, sizeof(u));
	return u;
}

/* Run the sink and the main-loop tick until the current item ends and
 * the queue has reacted. @returns 0 once something else (or nothing) plays. */
static int finish_current(void)
{
	char was[256];
	snprintf(was, sizeof(was), "%s", now());
	for (int i = 0; i < 3000; i++) {
		s_pull(s_ctx, s_buf, 480);
		if (queue_tick() & (PLAYER_EV_ADVANCED | PLAYER_EV_FINISHED)) return 0;
		struct timespec ts = { 0, 500000 };
		nanosleep(&ts, NULL);
	}
	fprintf(stderr, "  %s never finished\n", was);
	return -1;
}

static int has(const char *list, const char *line)
{
	return strstr(list, line) != NULL;
}

static int basics(void)
{
	char err[128], list[1024];
	CHECK(queue_play_at(0, err, sizeof(err)) != 0 && strstr(err, "empty"), "play on an empty queue says so");
	CHECK(queue_next(err, sizeof(err)) != 0, "next on an empty queue fails");

	CHECK(queue_add("sine:100:0.05") == 1, "first add is position 1");
	CHECK(queue_add("sine:200:0.05") == 2, "second add is position 2");
	CHECK(queue_add("sine:300:0.05") == 3, "third add is position 3");
	CHECK(queue_cursor() == 0 && player_state() == PLAYER_STOPPED, "adding does not start playback");

	CHECK(queue_play_at(0, err, sizeof(err)) == 0 && !strcmp(now(), "sine:100:0.05"), "play with no position starts at the top");
	queue_list(list, sizeof(list));
	CHECK(has(list, "> 1  sine:100:0.05\n") && has(list, "  2  sine:200:0.05\n"), "list marks the cursor");

	CHECK(finish_current() == 0 && !strcmp(now(), "sine:200:0.05"), "a natural end advances to the next item");
	CHECK(queue_cursor() == 2, "the cursor follows");
	CHECK(queue_next(err, sizeof(err)) == 0 && !strcmp(now(), "sine:300:0.05"), "next");
	CHECK(queue_next(err, sizeof(err)) != 0 && strstr(err, "end of queue"), "next past the end fails");
	CHECK(!strcmp(now(), "sine:300:0.05"), "a failed next leaves playback alone");
	CHECK(queue_prev(err, sizeof(err)) == 0 && !strcmp(now(), "sine:200:0.05"), "prev");
	CHECK(queue_prev(err, sizeof(err)) == 0 && queue_prev(err, sizeof(err)) != 0, "prev before the first item fails");

	CHECK(queue_play_at(3, err, sizeof(err)) == 0 && !strcmp(now(), "sine:300:0.05"), "jump to a position");
	CHECK(queue_play_at(9, err, sizeof(err)) != 0, "a position past the end is refused");
	CHECK(finish_current() == 0, "the last item ends");
	CHECK(player_state() == PLAYER_STOPPED && queue_cursor() == 3, "the end of the queue stops with the cursor on the last item");
	CHECK(queue_play_at(0, err, sizeof(err)) == 0 && !strcmp(now(), "sine:300:0.05"), "play after the end replays the cursor item");
	player_stop();
	return 0;
}

static int skipping(void)
{
	char err[128];
	queue_clear();
	queue_add("sine:100:0.05");
	queue_add("sine:-1");          /* raw refuses it */
	queue_add("nosuch:thing");     /* no source at all */
	queue_add("sine:400:0.05");
	CHECK(queue_play_at(1, err, sizeof(err)) == 0, "start");
	CHECK(finish_current() == 0 && !strcmp(now(), "sine:400:0.05"), "auto-advance skips items that cannot open");
	CHECK(queue_cursor() == 4, "the cursor lands on the item that played");
	CHECK(queue_play_at(2, err, sizeof(err)) != 0, "an explicit jump to a broken item reports the error");

	queue_clear();
	queue_add("sine:100:0.05");
	queue_add("sine:-1");
	CHECK(queue_play_at(1, err, sizeof(err)) == 0, "start");
	CHECK(finish_current() == 0 && player_state() == PLAYER_STOPPED, "a broken tail ends the queue quietly");
	return 0;
}

static int editing(void)
{
	char err[128], list[1024];
	queue_clear();
	queue_add("sine:100:10");
	queue_add("sine:200:10");
	queue_add("sine:300:10");
	queue_play_at(2, err, sizeof(err));

	CHECK(queue_remove(1) == 0 && queue_cursor() == 1 && !strcmp(now(), "sine:200:10"), "removing before the cursor shifts it, playback untouched");
	CHECK(queue_remove(1) == 0, "remove the playing item");
	CHECK(!strcmp(now(), "sine:200:10") && player_state() == PLAYER_PLAYING, "removing the playing item does not interrupt it");
	CHECK(queue_cursor() == 0, "the cursor is between items");
	queue_list(list, sizeof(list));
	CHECK(!strchr(list, '>') && has(list, "  1  sine:300:10\n"), "nothing is marked");
	CHECK(queue_next(err, sizeof(err)) == 0 && !strcmp(now(), "sine:300:10"), "next plays what followed the removed item");
	CHECK(queue_remove(5) != 0 && queue_remove(0) != 0, "bad positions are refused");

	/* play <uri> inserts after the cursor */
	queue_clear();
	queue_add("sine:100:10");
	queue_add("sine:200:10");
	queue_play_at(1, err, sizeof(err));
	CHECK(queue_play_uri("sine:150:10", err, sizeof(err)) == 0 && !strcmp(now(), "sine:150:10"), "play <uri> plays it now");
	queue_list(list, sizeof(list));
	CHECK(has(list, "  1  sine:100:10\n> 2  sine:150:10\n  3  sine:200:10\n"), "it is inserted right after the previous item");
	CHECK(queue_play_uri("sine:-1", err, sizeof(err)) != 0 && queue_length() == 3, "an unplayable URI is not left in the queue");
	CHECK(!strcmp(now(), "sine:150:10"), "and does not interrupt playback");

	queue_clear();
	CHECK(queue_length() == 0 && queue_cursor() == 0 && player_state() == PLAYER_PLAYING, "clear empties the queue but not the speaker");
	CHECK(queue_play_uri("sine:500:10", err, sizeof(err)) == 0 && queue_cursor() == 1, "play <uri> into an empty queue");
	player_stop();
	return 0;
}

static int gapless(void)
{
	char err[128];
	static float all[MM_PCM_CHANNELS * 48000];
	queue_clear();
	queue_add("sine:100:0.05");
	queue_add("sine:-1");        /* skipped at preload time */
	queue_add("sine:200:0.05");
	queue_add("sine:300:0.05");
	uint64_t base = player_underruns();
	CHECK(queue_play_at(1, err, sizeof(err)) == 0, "start");

	size_t got = 0;
	int advanced = 0, finished = 0;
	for (int i = 0; i < 3000 && !finished; i++) {
		s_pull(s_ctx, s_buf, 480);
		if (got + 480 <= sizeof(all) / sizeof(all[0]) / MM_PCM_CHANNELS) {
			memcpy(all + got * MM_PCM_CHANNELS, s_buf, sizeof(float) * MM_PCM_CHANNELS * 480);
			got += 480;
		}
		int ev = queue_tick();
		if (ev & PLAYER_EV_ADVANCED) {
			advanced++;
			if (advanced == 1) CHECK(queue_cursor() == 3 && !strcmp(now(), "sine:200:0.05"), "the cursor jumps over the skipped item when the speaker gets there");
		}
		if (ev & PLAYER_EV_FINISHED) finished = 1;
		struct timespec ts = { 0, 300000 };
		nanosleep(&ts, NULL);
	}
	CHECK(finished && advanced == 2, "two gapless advances, then the end");
	CHECK(queue_cursor() == 4, "the cursor ends on the last item");

	long first = -1, last = -1, run = 0, worst = 0;
	for (size_t f = 0; f < got; f++) {
		if (all[f * MM_PCM_CHANNELS] != 0.0f) { if (first < 0) first = (long)f; last = (long)f; }
	}
	for (long f = first; f <= last; f++) {
		run = all[f * MM_PCM_CHANNELS] == 0.0f ? run + 1 : 0;
		if (run > worst) worst = run;
	}
	CHECK(last - first + 2 == 3 * 2400, "three 2400-frame tones span exactly 7200 frames");
	CHECK(worst <= 1, "no silence between items (a lone zero is a tone's first sample)");
	CHECK(player_underruns() == base, "no underruns");
	return 0;
}

static int edits_drop_preload(void)
{
	char err[128];
	queue_clear();
	queue_add("sine:100:5");
	CHECK(queue_play_at(1, err, sizeof(err)) == 0, "a 5 s item, nothing after it");
	for (int i = 0; i < 5; i++) { s_pull(s_ctx, s_buf, 480); queue_tick(); }
	CHECK(player_preload_state() == 0, "nothing to preload yet");
	queue_add("sine:200:5");
	queue_tick();
	CHECK(player_preload_state() == 1, "adding an item gets it preloaded");
	queue_play_uri("sine:150:5", err, sizeof(err));
	queue_tick();
	queue_tick();
	CHECK(player_preload_state() == 1, "after an insert the preload is redone");
	char st[1024];
	queue_list(st, sizeof(st));
	CHECK(strstr(st, "> 2  sine:150:5\n  3  sine:200:5\n") != NULL, "it is the item now after the cursor");
	queue_remove(3);
	CHECK(player_preload_state() == 0, "removing the preloaded item drops it");
	queue_tick();
	CHECK(player_preload_state() == 0, "and with nothing left after the cursor nothing is preloaded");
	player_stop();
	return 0;
}

static int verbs(void)
{
	CHECK(ipc_has_verb("play") && ipc_has_verb("next") && ipc_has_verb("prev") && ipc_has_verb("queue"), "queue verbs registered");
	return 0;
}

static int lists(void)
{
	char err[256], **v;
	queue_clear();

	/* Expansion: in order, into nested lists, past a broken one and a loop. */
	g_lists = 0;
	long n = catalog_tracks("box:a", &v, 1000, err, sizeof(err));
	CHECK(n == 3 && !strcmp(v[0], "sine:300:5") && !strcmp(v[1], "sine:350:5") && !strcmp(v[2], "sine:400:5"),
	      "a list's tracks, nested ones in place, a failing one skipped");
	CHECK(g_lists > 3 && g_lists < 20, "a list containing itself stops at the depth limit");
	CHECK(g_flags == MM_LIST_URIS, "expansion asks for URIs only");
	catalog_free(v, n);
	CHECK((n = catalog_tracks("box:a", &v, 2, err, sizeof(err))) == 2, "capped at max");
	catalog_free(v, n);
	CHECK((n = catalog_tracks("box:t", &v, 10, err, sizeof(err))) == 1 && !strcmp(v[0], "box:t"), "a track is itself");
	catalog_free(v, n);
	CHECK((n = catalog_tracks("sine:500", &v, 10, err, sizeof(err))) == 1, "no catalog for the scheme: itself");
	catalog_free(v, n);
	CHECK(catalog_tracks("box:empty", &v, 10, err, sizeof(err)) == 0, "an empty list is no tracks");
	free(v);
	CHECK(catalog_tracks("box:nope", &v, 10, err, sizeof(err)) < 0 && strstr(err, "no such box"), "the plugin's error");

	/* browse */
	CHECK(run("browse", "box:", NULL) == MM_REPLY_RAW && !strcmp(R, "list\tbox:a\tA\tArtist Tab\t-\t-\nlist\tbox:empty\tEmpty\tArtist Tab\t-\t-\n"),
	      "the top level");
	CHECK(run("browse", "box:b", NULL) == MM_REPLY_RAW && !strcmp(R, "track\tsine:350:5\tThree-fifty\tArtist Tab\t-\t1:01\n"),
	      "a track line, a tab in a field made a space");
	CHECK(g_flags == 0, "browse asks for everything");
	CHECK(run("browse", "box:empty", NULL) == MM_REPLY_RAW && !strcmp(R, "empty\n"), "empty");
	CHECK(run("browse", "box:t", NULL) == MM_ERR && strstr(R, "is a track"), "a track is not browsable");
	CHECK(run("browse", "nosuch:", NULL) == MM_ERR && strstr(R, "nothing lists"), "unknown scheme");

	/* queue add and play take lists */
	CHECK(run("queue", "add", "box:a") == MM_OK && !strcmp(R, "added 1-3"), "queue add of a list");
	CHECK(run("queue", "add", "box:empty") == MM_ERR && strstr(R, "nothing to add"), "an empty list adds nothing");
	CHECK(run("play", "box:b", NULL) == MM_OK && !strcmp(now(), "sine:350:5") && queue_length() == 4,
	      "play of a list plays its first track");
	CHECK(run("play", "box:dud", NULL) == MM_ERR && queue_length() == 4 && !strcmp(now(), "sine:350:5"),
	      "an unplayable list leaves queue and playback alone");
	queue_clear();
	CHECK(run("play", "box:a", NULL) == MM_OK && queue_length() == 3 && queue_cursor() == 1, "play a list into an empty queue");
	player_stop();
	queue_clear();
	return 0;
}

int main(void)
{
	config_override("host.sink=testsink");
	config_load(NULL);
	loader_add(&testsink, NULL, "test");
	loader_add(mm_static_entry_raw(), NULL, "builtin");
	loader_add(&boxcat, NULL, "test");
	loader_init_all();
	CHECK(player_init() == 0, "player");
	CHECK(queue_init() == 0, "queue");
	CHECK(catalog_init() == 0, "catalog");

	RUN(verbs);
	RUN(basics);
	RUN(skipping);
	RUN(editing);
	RUN(gapless);
	RUN(edits_drop_preload);
	RUN(lists);

	queue_fini();
	CHECK(!ipc_has_verb("queue"), "fini drops the verbs");
	player_fini();
	loader_fini_all();
	config_free();
	CHECK_PASS("test-queue");
}

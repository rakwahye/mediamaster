/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The queue driving the real player, raw source and a hand-driven sink.
 * Items are short tones whose frequency names them, so "what is playing"
 * is read from the player's current URI. */
#include "queue.h"
#include "catalog.h"
#include "radio.h"
#include "player.h"
#include "config.h"
#include "ipc.h"
#include "loader.h"
#include "check.h"
#include <stdatomic.h>
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
static const struct mm_sink_ops ts_ops = { .size = sizeof(struct mm_sink_ops), .start = ts_start, .stop = ts_stop };
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
 *   box:dud         a list whose only track no source plays
 *   box:search:<q>  "three": a list, then sine:300 and sine:350; else nothing
 * and a radio op: after sine:100:*, sine:100 again, sine:610, sine:620, a
 * list, sine:630; after anything else, an error.
 * "alt" is a second radio provider: always sine:700. */
/* Written by list, which radio calls on its own thread. */
static atomic_int g_lists;       /* list calls, to see expansion stop */
static atomic_int g_radio_calls;
static atomic_uint g_flags;      /* flags of the last list call */
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
	} else if (!strncmp(uri, "box:search:", 11)) {
		if (strcmp(uri + 11, "three") != 0) return 0;
		if (emit_list(emit, ctx, MM_ITEM_LIST, "box:b", "B")) return 0;
		if (emit_list(emit, ctx, MM_ITEM_TRACK, "sine:300:5", "Three")) return 0;
		emit_list(emit, ctx, MM_ITEM_TRACK, "sine:350:5", "Three-fifty");
	} else if (!strcmp(uri, "box:t")) {
		return MM_NOT_A_LIST;
	} else if (strcmp(uri, "box:empty") != 0) {
		snprintf(err, errlen, "no such box: %s", uri);
		return -1;
	}
	return 0;
}
static _Atomic uint64_t g_seed_ms; /* the duration the last seed came with */
static int box_radio(void *priv, const struct mm_item *seed, uint32_t flags, mm_item_fn emit, void *ctx,
                     char *err, size_t errlen)
{
	(void)priv; (void)flags;
	g_radio_calls++;
	g_seed_ms = seed->duration_ms;
	if (strncmp(seed->uri, "sine:100", 8) != 0) {
		snprintf(err, errlen, "no radio for %s", seed->uri);
		return -1;
	}
	if (emit_list(emit, ctx, MM_ITEM_TRACK, seed->uri, "Seed")) return 0;
	if (emit_list(emit, ctx, MM_ITEM_TRACK, "sine:610:0.05", "R1")) return 0;
	if (emit_list(emit, ctx, MM_ITEM_TRACK, "sine:620:0.05", "R2")) return 0;
	if (emit_list(emit, ctx, MM_ITEM_LIST, "box:b", "B")) return 0;
	emit_list(emit, ctx, MM_ITEM_TRACK, "sine:630:0.05", "R3");
	return 0;
}
static int box_init(const struct mm_host *h, void **priv) { (void)h; *priv = NULL; return 0; }
static const struct mm_catalog_ops box_ops = { .size = sizeof(struct mm_catalog_ops), .list = box_list, .radio = box_radio };
static const struct mm_plugin boxcat = { .abi = MM_ABI, .name = "box", .schemes = "box", .init = box_init, .catalog = &box_ops };

static atomic_int g_alt_calls;
static int alt_list(void *priv, const char *uri, uint32_t flags, mm_item_fn emit, void *ctx, char *err, size_t errlen)
{
	(void)priv; (void)uri; (void)flags; (void)emit; (void)ctx;
	snprintf(err, errlen, "alt lists nothing");
	return -1;
}
static int alt_radio(void *priv, const struct mm_item *seed, uint32_t flags, mm_item_fn emit, void *ctx,
                     char *err, size_t errlen)
{
	(void)priv; (void)seed; (void)flags; (void)err; (void)errlen;
	g_alt_calls++;
	emit_list(emit, ctx, MM_ITEM_TRACK, "sine:700:0.05", "Alt");
	return 0;
}
static const struct mm_catalog_ops alt_ops = { .size = sizeof(struct mm_catalog_ops), .list = alt_list, .radio = alt_radio };
/* "shelf" owns two recordings, by title. */
static int shelf_match(void *priv, const struct mm_item *want, char *uri, size_t cap)
{
	(void)priv;
	const char *mine = !want->title ? NULL : !strcmp(want->title, "Three") ? "sine:301:5"
	                 : !strcmp(want->title, "R1") ? "sine:611:0.05" : NULL;
	if (!mine || !want->artist || want->duration_ms != 61000) return 0;
	snprintf(uri, cap, "%s", mine);
	return 1;
}
static const struct mm_catalog_ops shelf_ops = { .size = sizeof(struct mm_catalog_ops), .list = alt_list, .match = shelf_match };
static const struct mm_plugin shelfcat = { .abi = MM_ABI, .name = "shelf", .schemes = "shelf", .init = box_init, .catalog = &shelf_ops };
static const struct mm_plugin altcat = { .abi = MM_ABI, .name = "alt", .schemes = "alt", .init = box_init, .catalog = &alt_ops };

static char R[1 << 16];
static int run(const char *a0, const char *a1, const char *a2)
{
	char *argv[4] = { (char *)a0, (char *)a1, (char *)a2, NULL };
	int argc = a2 ? 3 : a1 ? 2 : 1;
	return ipc_dispatch(argc, argv, NULL, 0, R, sizeof(R));
}

static int run4(const char *a0, const char *a1, const char *a2, const char *a3)
{
	char *argv[5] = { (char *)a0, (char *)a1, (char *)a2, (char *)a3, NULL };
	return ipc_dispatch(4, argv, NULL, 0, R, sizeof(R));
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

/* queue mv: positions as the list shows them; the cursor and a preload
 * follow their items. */
static int moving(void)
{
	char err[128], list[1024];
	queue_clear();
	queue_add("sine:100:10");
	queue_add("sine:200:10");
	queue_add("sine:300:10");
	queue_add("sine:400:10");
	queue_add("sine:500:10");
	queue_play_at(2, err, sizeof(err));

	CHECK(queue_move(5, 1) == 0 && queue_cursor() == 3 && !strcmp(now(), "sine:200:10"), "moving an item from below to above the cursor shifts it down");
	queue_list(list, sizeof(list));
	CHECK(has(list, "  1  sine:500:10\n  2  sine:100:10\n> 3  sine:200:10\n  4  sine:300:10\n  5  sine:400:10\n"), "the list reads as moved");
	CHECK(queue_move(1, 5) == 0 && queue_cursor() == 2, "and back up again");
	CHECK(queue_move(2, 4) == 0 && queue_cursor() == 4 && !strcmp(now(), "sine:200:10"), "the playing item itself moves, the cursor follows, playback untouched");
	queue_list(list, sizeof(list));
	CHECK(has(list, "  1  sine:100:10\n  2  sine:300:10\n  3  sine:400:10\n> 4  sine:200:10\n  5  sine:500:10\n"), "it sits where asked");
	CHECK(queue_move(3, 3) == 0 && queue_cursor() == 4, "moving onto itself changes nothing");
	CHECK(queue_move(1, 2) == 0 && queue_cursor() == 4, "a move entirely above the cursor leaves it");
	CHECK(queue_move(5, 5) == 0 && queue_move(0, 1) != 0 && queue_move(1, 6) != 0 && queue_move(6, 1) != 0, "bad positions are refused");
	CHECK(queue_next(err, sizeof(err)) == 0 && !strcmp(now(), "sine:500:10"), "next plays what now follows the cursor");

	/* The verb: mv <n> <m>, and mv <n> next for "play this next". */
	queue_clear();
	queue_add("sine:100:10");
	queue_add("sine:200:10");
	queue_add("sine:300:10");
	queue_add("sine:400:10");
	queue_play_at(1, err, sizeof(err));
	CHECK(run4("queue", "mv", "4", "2") == MM_OK && !strcmp(R, "moved 4 to 2"), "mv <n> <m>");
	queue_list(list, sizeof(list));
	CHECK(has(list, "> 1  sine:100:10\n  2  sine:400:10\n  3  sine:200:10\n  4  sine:300:10\n"), "and it is there");
	CHECK(run4("queue", "mv", "4", "next") == MM_OK && !strcmp(R, "moved 4 to 2"), "mv <n> next puts it right after the cursor");
	queue_list(list, sizeof(list));
	CHECK(has(list, "> 1  sine:100:10\n  2  sine:300:10\n  3  sine:400:10\n  4  sine:200:10\n"), "the last item is now next");
	queue_play_at(3, err, sizeof(err));
	CHECK(run4("queue", "mv", "1", "next") == MM_OK && !strcmp(R, "moved 1 to 3"), "an item from above the cursor lands after it, one position earlier");
	queue_list(list, sizeof(list));
	CHECK(has(list, "  1  sine:300:10\n> 2  sine:400:10\n  3  sine:100:10\n  4  sine:200:10\n"), "it follows the playing item");
	CHECK(run4("queue", "mv", "9", "1") == MM_ERR && !strcmp(R, "no item 9"), "a bad source");
	CHECK(run4("queue", "mv", "1", "9") == MM_ERR && !strcmp(R, "no item 9"), "a bad target");
	CHECK(run4("queue", "mv", "1", "there") == MM_ERR && strstr(R, "usage: queue mv"), "a bad word");
	player_stop();
	return 0;
}

/* A move is an edit: a preload not yet playing is dropped and the next
 * tick preloads whatever now follows the cursor. */
static int moving_preload(void)
{
	char err[128], list[1024];
	queue_clear();
	queue_add("sine:100:5");
	queue_add("sine:200:5");
	queue_add("sine:300:5");
	CHECK(queue_play_at(1, err, sizeof(err)) == 0, "play the first of three");
	queue_tick();
	CHECK(player_preload_state() == 1, "the second is preloaded");
	queue_move(1, 2);
	CHECK(player_preload_state() == 0, "moving the playing item past the preloaded one drops the preload");
	queue_list(list, sizeof(list));
	CHECK(has(list, "  1  sine:200:5\n> 2  sine:100:5\n  3  sine:300:5\n"), "the cursor followed its item");
	queue_tick();
	CHECK(player_preload_state() == 1, "and the item now after the cursor is preloaded");
	queue_move(2, 3);
	CHECK(player_preload_state() == 0, "the playing item moved to the end: nothing left to preload");
	queue_tick();
	CHECK(player_preload_state() == 0 && queue_cursor() == 3, "still nothing");
	player_stop();
	return 0;
}

/* queue add with browse/search lines, queue list -l. */
static int labels(void)
{
	char reply[2048], l[64];
	queue_clear();
	char *add[] = { "queue", "add", "track\tsine:100:0.05\tLow\tOsc\t-\t0:05", "sine:200:0.05",
	                "list\tbox:b\tB\t-\t-\t-", NULL };
	CHECK(ipc_dispatch(5, add, NULL, 0, reply, sizeof(reply)) == MM_OK && !strcmp(R, "added 1-3"),
	      "add takes a search line, a URI and a list line");
	char *ls[] = { "queue", "list", "-l", NULL };
	CHECK(ipc_dispatch(3, ls, NULL, 0, reply, sizeof(reply)) == MM_REPLY_RAW, "list -l");
	CHECK(has(reply, "1\tsine:100:0.05\tLow\tOsc\t-\t0:05\n"), "a track line's fields label it");
	CHECK(has(reply, "2\tsine:200:0.05\t-\t-\t-\t-\n"), "a bare URI has no label");
	CHECK(has(reply, "3\tsine:350:5\tThree-fifty\tArtist Tab\t-\t1:01\n"),
	      "a list line queues the list's tracks, with the titles the catalog gave");
	char err[64];
	CHECK(queue_play_at(1, err, sizeof(err)) == 0, "play the labelled item");
	CHECK(ipc_dispatch(3, ls, NULL, 0, reply, sizeof(reply)) == MM_REPLY_RAW &&
	      has(reply, ">1\tsine:100:0.05\tLow\t"), "> marks the cursor");
	player_stop();

	char *bad[] = { "queue", "add", "track\t\tNo URI", NULL };
	CHECK(ipc_dispatch(3, bad, NULL, 0, reply, sizeof(reply)) == MM_ERR, "a line without a URI is refused");
	char *badl[] = { "queue", "list", "-x", NULL };
	CHECK(ipc_dispatch(3, badl, NULL, 0, reply, sizeof(reply)) == MM_ERR, "list takes only -l");

	queue_label_make(l, sizeof(l), "a\tb", "-", NULL, "1:01");
	CHECK(!strcmp(l, "a b\t\t\t1:01"), "label fields: tabs made spaces, - read as empty");
	queue_label_make(l, sizeof(l), "-", "", NULL, "-");
	CHECK(!l[0], "an all-empty label is none");
	queue_clear();
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
	CHECK(run("queue", "list", "-l") == MM_REPLY_RAW &&
	      !strcmp(R, "1\tsine:300:5\tThree\tArtist Tab\t-\t1:01\n2\tsine:350:5\tThree-fifty\tArtist Tab\t-\t1:01\n"
	                 "3\tsine:400:5\tFour\tArtist Tab\t-\t1:01\n"),
	      "a list's tracks keep the titles the catalog gave");
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

static int search(void)
{
	CHECK(catalog_is_query("daft punk") && catalog_is_query("nosuch:thing") && catalog_is_query("three"),
	      "words, and a scheme nothing claims, are a query");
	CHECK(!catalog_is_query("/music/a.flac") && !catalog_is_query("sine:440") && !catalog_is_query("box:a") &&
	      !catalog_is_query("file:/x") && !catalog_is_query(""), "paths and claimed URIs are not");

	CHECK(run("search", "three", NULL) == MM_REPLY_RAW &&
	      !strcmp(R, "list\tbox:b\tB\tArtist Tab\t-\t-\n"
	                 "track\tsine:300:5\tThree\tArtist Tab\t-\t1:01\n"
	                 "track\tsine:350:5\tThree-fifty\tArtist Tab\t-\t1:01\n"), "every source's results");
	config_override("catalog.search_max=2");
	config_reload();
	CHECK(run("search", "three", NULL) == MM_REPLY_RAW && !strstr(R, "sine:350"), "capped per source");
	config_override("catalog.search_max=20");
	config_reload();
	CHECK(run("search", "nothing", NULL) == MM_REPLY_RAW && !strcmp(R, "no results\n"), "no results");
	CHECK(run("search", NULL, NULL) == MM_ERR, "search needs words");

	queue_clear();
	CHECK(run("play", "three", NULL) == MM_OK && !strcmp(now(), "sine:300:5") && queue_length() == 1,
	      "play <words>: the first track found, not the list before it");
	CHECK(run("play", "nothing", NULL) == MM_ERR && strstr(R, "nothing found"), "play <words> with no hit");
	player_stop();
	queue_clear();
	return 0;
}

/* Tick the main loop (radio included) until cond or ~2 s. */
#define TICK_UNTIL(cond) do { \
	for (int _i = 0; _i < 400 && !(cond); _i++) { \
		s_pull(s_ctx, s_buf, 480); queue_tick(); radio_tick(); \
		struct timespec _ts = { 0, 5000000 }; nanosleep(&_ts, NULL); \
	} } while (0)

static int radio(void)
{
	char err[256], list[2048];
	queue_clear();
	g_radio_calls = 0;
	CHECK(run("radio", NULL, NULL) == MM_OK && !strcmp(R, "on"), "on by default");

	/* One track queued: radio adds more like it, minus itself, the list
	 * and what is queued already. */
	CHECK(queue_add("sine:620:0.05") == 1, "queue a track radio will also name");
	CHECK(queue_play_uri("sine:100:0.3", err, sizeof(err)) == 0, "play the seed after it");
	CHECK(queue_cursor() == 2 && queue_length() == 2, "the seed is the last item");
	TICK_UNTIL(queue_length() == 4);
	queue_list(list, sizeof(list));
	CHECK(queue_length() == 4 && strstr(list, "sine:610:0.05") && strstr(list, "sine:630:0.05"),
	      "610 and 630 added; the seed, the queued 620 and the list left out");
	char llist[4096];
	queue_list_long(llist, sizeof(llist));
	CHECK(strstr(llist, "\tsine:610:0.05\tR1\tArtist Tab\t-\t1:01\n") && strstr(llist, "\tsine:630:0.05\tR3\t"),
	      "radio's tracks come titled");
	CHECK(g_radio_calls == 1, "asked once");
	CHECK(g_seed_ms == 300, "the seed came with its duration");
	CHECK(g_alt_calls == 0, "the first provider that answered won");
	for (int i = 0; i < 20; i++) radio_tick();
	CHECK(g_radio_calls == 1, "an item seeds radio once");

	/* Playback carries on into them. */
	TICK_UNTIL(!strcmp(now(), "sine:610:0.05"));
	CHECK(!strcmp(now(), "sine:610:0.05"), "the seed ended into the first radio track");

	/* Off: the last item asks for nothing. */
	CHECK(run("radio", "off", NULL) == MM_OK && !strcmp(R, "off"), "off");
	int before = g_radio_calls;
	CHECK(queue_play_at(4, err, sizeof(err)) == 0, "play the last item");
	TICK_UNTIL(0);
	CHECK(g_radio_calls == before, "radio off: nothing asked");
	CHECK(run("radio", "maybe", NULL) == MM_ERR, "usage");
	CHECK(run("radio", "on", NULL) == MM_OK, "back on");

	/* box has nothing for this seed: the next provider, alt, fills in. */
	queue_clear();
	CHECK(queue_play_uri("sine:440:0.3", err, sizeof(err)) == 0, "a seed box has no radio for");
	TICK_UNTIL(queue_length() == 2);
	queue_list(list, sizeof(list));
	CHECK(g_radio_calls == before + 1 && g_alt_calls == 1 && strstr(list, "sine:700"), "box asked, then alt");

	/* radio.order puts alt first; then only box: nothing for this seed. */
	config_override("radio.order=alt, box");
	config_reload();
	queue_clear();
	CHECK(queue_play_uri("sine:100:0.3", err, sizeof(err)) == 0, "a seed both could answer");
	TICK_UNTIL(queue_length() == 2);
	queue_list(list, sizeof(list));
	CHECK(g_alt_calls == 2 && strstr(list, "sine:700") && !strstr(list, "sine:610"), "the order is followed");
	config_override("radio.order=box");
	config_reload();
	queue_clear();
	CHECK(queue_play_uri("sine:440:0.3", err, sizeof(err)) == 0, "only box, which has nothing");
	int bc = g_radio_calls;
	TICK_UNTIL(g_radio_calls > bc);
	TICK_UNTIL(0);
	CHECK(queue_length() == 1 && g_alt_calls == 2, "nothing added; providers not named are not asked");
	config_override("radio.order=");
	config_reload();
	player_stop();
	queue_clear();
	radio_fini();
	return 0;
}

static int own(void)
{
	char llist[4096];
	queue_clear();
	config_override("own.prefer=shelf");
	config_reload();

	/* Expansion: asks with titles now, and the owned track is swapped. */
	CHECK(run("queue", "add", "box:a") == MM_OK && !strcmp(R, "added 1-3"), "queue a list");
	CHECK(g_flags == 0, "with an owner to match against, lists are asked with titles");
	queue_list_long(llist, sizeof(llist));
	CHECK(!strncmp(llist, "1\tsine:301:5\tThree\t", 19) && strstr(llist, "2\tsine:350:5\t"),
	      "the owned copy in its place, the label kept; the rest as they were");

	/* Radio: R1 is owned. */
	queue_clear();
	char err[128];
	CHECK(queue_play_uri("sine:100:0.3", err, sizeof(err)) == 0, "a seed");
	TICK_UNTIL(queue_length() >= 4);
	queue_list_long(llist, sizeof(llist));
	CHECK(strstr(llist, "\tsine:611:0.05\tR1\t") && !strstr(llist, "sine:610"), "radio's track swapped for your copy");

	config_override("own.prefer=off");
	config_reload();
	queue_clear();
	CHECK(run("queue", "add", "box:a") == MM_OK && g_flags == MM_LIST_URIS, "off: URIs only again");
	queue_list_long(llist, sizeof(llist));
	CHECK(!strncmp(llist, "1\tsine:300:5\t", 13), "and nothing swapped");
	player_stop();
	queue_clear();
	radio_fini(); /* join the fetch radio started */
	return 0;
}

int main(void)
{
	config_override("host.sink=testsink");
	config_load(NULL);
	loader_add(&testsink, NULL, "test");
	loader_add(mm_static_entry_raw(), NULL, "builtin");
	loader_add(&boxcat, NULL, "test");
	loader_add(&altcat, NULL, "test");
	loader_add(&shelfcat, NULL, "test");
	loader_init_all();
	CHECK(player_init() == 0, "player");
	CHECK(queue_init() == 0, "queue");
	CHECK(catalog_init() == 0, "catalog");
	CHECK(radio_init() == 0, "radio");

	RUN(verbs);
	RUN(basics);
	RUN(skipping);
	RUN(editing);
	RUN(moving);
	RUN(moving_preload);
	RUN(gapless);
	RUN(edits_drop_preload);
	RUN(lists);
	RUN(search);
	RUN(radio);
	RUN(own);
	RUN(labels);

	queue_fini();
	CHECK(!ipc_has_verb("queue"), "fini drops the verbs");
	player_fini();
	loader_fini_all();
	config_free();
	CHECK_PASS("test-queue");
}

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file queue.c
 * @brief Play queue and cursor.
 *
 * Lock order: queue lock, then (inside player_*) the player lock. The
 * player never calls back into the queue, so this cannot invert. */

#include "queue.h"
#include "ipc.h"
#include "log.h"
#include "player.h"
#include "mm_plugin.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "queue"
#define QUEUE_MAX 100000

static struct {
	pthread_mutex_t lock;
	char **uri;
	size_t n, cap;
	long cur;         /* 0-based index; -1 = none */
	int cur_removed;  /* the item at `cur` was deleted; cur now names its successor */
	long pre;         /* index handed to the player as the next item */
	int pre_valid;
	int pre_removed;  /* like cur_removed, for pre */
} Q = { .lock = PTHREAD_MUTEX_INITIALIZER, .cur = -1 };

/* The list changed. A preload the reader has not started on is stale:
 * drop it, and the player will ask again. One it already reads from
 * plays on, and `pre` keeps tracking it. */
static void edited(void)
{
	if (Q.pre_valid && player_preload_state() == 2) return;
	player_drop_preload();
	Q.pre_valid = 0;
}

/* ------------------------------------------------------------------ storage (lock held) */

static int insert_at(size_t i, const char *uri)
{
	if (Q.n >= QUEUE_MAX) return -1;
	if (Q.n == Q.cap) {
		size_t cap = Q.cap ? Q.cap * 2 : 64;
		char **v = realloc(Q.uri, cap * sizeof(*v));
		if (!v) return -1;
		Q.uri = v;
		Q.cap = cap;
	}
	char *d = strdup(uri);
	if (!d) return -1;
	memmove(Q.uri + i + 1, Q.uri + i, (Q.n - i) * sizeof(*Q.uri));
	Q.uri[i] = d;
	Q.n++;
	if (Q.cur >= 0 && (long)i <= Q.cur && !(Q.cur_removed && (long)i == Q.cur)) Q.cur++;
	if (Q.pre_valid && (long)i <= Q.pre && !(Q.pre_removed && (long)i == Q.pre)) Q.pre++;
	return 0;
}

/* Try to play index i. On success the cursor moves there. */
static int play_index(long i, char *err, size_t errlen)
{
	if (i < 0 || (size_t)i >= Q.n) {
		snprintf(err, errlen, "no item %ld", i + 1);
		return -1;
	}
	if (player_play(Q.uri[i], err, errlen) != 0) return -1;
	Q.cur = i;
	Q.cur_removed = 0;
	Q.pre_valid = 0; /* player_play dropped any preload */
	return 0;
}

/* From index i in direction dir, play the first item that opens.
 * @returns 0, or -1 when none did (cursor parked on the last tried). */
static int play_from(long i, int dir)
{
	char err[256];
	for (; i >= 0 && (size_t)i < Q.n; i += dir) {
		if (play_index(i, err, sizeof(err)) == 0) return 0;
		LOGW(TAG, "skipping %ld (%s): %s", i + 1, Q.uri[i], err);
		Q.cur = i;
		Q.cur_removed = 0;
	}
	return -1;
}

/* ------------------------------------------------------------------ API */

long queue_add(const char *uri)
{
	pthread_mutex_lock(&Q.lock);
	long pos = insert_at(Q.n, uri) == 0 ? (long)Q.n : -1;
	if (pos > 0) edited();
	pthread_mutex_unlock(&Q.lock);
	return pos;
}

int queue_play_uri(const char *uri, char *err, size_t errlen)
{
	pthread_mutex_lock(&Q.lock);
	/* After the cursor; if the cursor's item was removed, at its slot. */
	size_t at = Q.cur < 0 ? Q.n : (size_t)(Q.cur + (Q.cur_removed ? 0 : 1));
	if (at > Q.n) at = Q.n;
	int rc = -1;
	if (insert_at(at, uri) != 0) {
		snprintf(err, errlen, "queue full");
	} else if ((rc = play_index((long)at, err, errlen)) != 0) {
		/* Not playable: do not leave it in the queue. */
		free(Q.uri[at]);
		memmove(Q.uri + at, Q.uri + at + 1, (Q.n - at - 1) * sizeof(*Q.uri));
		Q.n--;
		if (Q.cur > (long)at) Q.cur--;
		if (Q.pre_valid && Q.pre > (long)at) Q.pre--;
		edited();
	}
	pthread_mutex_unlock(&Q.lock);
	return rc;
}

int queue_play_at(long pos, char *err, size_t errlen)
{
	pthread_mutex_lock(&Q.lock);
	long i = pos > 0 ? pos - 1 : (Q.cur >= 0 ? Q.cur : 0);
	int rc;
	if (!Q.n) {
		snprintf(err, errlen, "queue is empty");
		rc = -1;
	} else {
		rc = play_index(i, err, errlen);
	}
	pthread_mutex_unlock(&Q.lock);
	return rc;
}

int queue_next(char *err, size_t errlen)
{
	pthread_mutex_lock(&Q.lock);
	long i = Q.cur < 0 ? 0 : Q.cur + (Q.cur_removed ? 0 : 1);
	int rc;
	if ((size_t)i >= Q.n) {
		snprintf(err, errlen, "end of queue");
		rc = -1;
	} else {
		rc = play_index(i, err, errlen);
	}
	pthread_mutex_unlock(&Q.lock);
	return rc;
}

int queue_prev(char *err, size_t errlen)
{
	pthread_mutex_lock(&Q.lock);
	long i = Q.cur - 1;
	int rc;
	if (Q.cur < 0 || i < 0) {
		snprintf(err, errlen, "start of queue");
		rc = -1;
	} else {
		rc = play_index(i, err, errlen);
	}
	pthread_mutex_unlock(&Q.lock);
	return rc;
}

int queue_remove(long pos)
{
	pthread_mutex_lock(&Q.lock);
	int rc = -1;
	if (pos >= 1 && (size_t)pos <= Q.n) {
		size_t i = (size_t)pos - 1;
		free(Q.uri[i]);
		memmove(Q.uri + i, Q.uri + i + 1, (Q.n - i - 1) * sizeof(*Q.uri));
		Q.n--;
		if ((long)i < Q.cur) Q.cur--;
		else if ((long)i == Q.cur) Q.cur_removed = 1;
		if (Q.pre_valid) {
			if ((long)i < Q.pre) Q.pre--;
			else if ((long)i == Q.pre) Q.pre_removed = 1;
		}
		edited();
		rc = 0;
	}
	pthread_mutex_unlock(&Q.lock);
	return rc;
}

void queue_clear(void)
{
	pthread_mutex_lock(&Q.lock);
	for (size_t i = 0; i < Q.n; i++) free(Q.uri[i]);
	Q.n = 0;
	Q.cur = -1;
	Q.cur_removed = 0;
	if (Q.pre_valid) { Q.pre = 0; Q.pre_removed = 1; }
	edited();
	pthread_mutex_unlock(&Q.lock);
}

size_t queue_length(void)
{
	pthread_mutex_lock(&Q.lock);
	size_t n = Q.n;
	pthread_mutex_unlock(&Q.lock);
	return n;
}

long queue_cursor(void)
{
	pthread_mutex_lock(&Q.lock);
	long c = (Q.cur >= 0 && !Q.cur_removed && (size_t)Q.cur < Q.n) ? Q.cur + 1 : 0;
	pthread_mutex_unlock(&Q.lock);
	return c;
}

/* Lock held. Give the player the next item that opens, after the cursor. */
static void preload_next(void)
{
	long i = Q.cur < 0 ? 0 : Q.cur + (Q.cur_removed ? 0 : 1);
	char err[256];
	for (; (size_t)i < Q.n; i++) {
		if (player_preload(Q.uri[i], err, sizeof(err)) == 0) {
			Q.pre = i;
			Q.pre_valid = 1;
			Q.pre_removed = 0;
			return;
		}
		LOGW(TAG, "skipping %ld (%s): %s", i + 1, Q.uri[i], err);
	}
}

static void on_advanced(void)
{
	if (!Q.pre_valid) return;
	Q.cur = Q.pre;
	Q.cur_removed = Q.pre_removed;
	Q.pre_valid = 0;
}

static void on_finished_locked(void);

int queue_tick(void)
{
	int ev = player_tick();
	pthread_mutex_lock(&Q.lock);
	if (ev & PLAYER_EV_ADVANCED) on_advanced();
	if (ev & PLAYER_EV_FINISHED) on_finished_locked();
	if (player_wants_next()) preload_next();
	pthread_mutex_unlock(&Q.lock);
	return ev;
}

void queue_on_finished(void)
{
	pthread_mutex_lock(&Q.lock);
	on_finished_locked();
	pthread_mutex_unlock(&Q.lock);
}

static void on_finished_locked(void)
{
	Q.pre_valid = 0;
	long i = Q.cur < 0 ? 0 : Q.cur + (Q.cur_removed ? 0 : 1);
	if ((size_t)i < Q.n) {
		if (play_from(i, 1) != 0) LOGI(TAG, "nothing playable left in the queue");
	} else if (Q.n) {
		LOGI(TAG, "end of queue");
	}
}

void queue_list(char *buf, size_t cap)
{
	size_t off = 0;
	if (cap) buf[0] = '\0';
	pthread_mutex_lock(&Q.lock);
	for (size_t i = 0; i < Q.n && off < cap; i++) {
		int mark = (long)i == Q.cur && !Q.cur_removed;
		int w = snprintf(buf + off, cap - off, "%s%zu  %s\n", mark ? "> " : "  ", i + 1, Q.uri[i]);
		if (w < 0) break;
		off += (size_t)w;
	}
	pthread_mutex_unlock(&Q.lock);
	if (off >= cap && cap > 4) memcpy(buf + cap - 5, "...\n", 5);
}

/* ------------------------------------------------------------------ verbs */

static void join_args(int argc, char *const *argv, int from, char *out, size_t cap)
{
	size_t off = 0;
	out[0] = '\0';
	for (int i = from; i < argc && off < cap; i++)
		off += (size_t)snprintf(out + off, cap - off, "%s%s", i > from ? " " : "", argv[i]);
}

static int parse_pos(const char *s, long *out)
{
	char *end;
	long v = strtol(s, &end, 10);
	if (end == s || *end || v < 1) return -1;
	*out = v;
	return 0;
}

static int v_play(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	char err[256];
	if (c->argc < 2) {
		if (player_state() == PLAYER_PAUSED) {
			player_pause(0);
			snprintf(c->reply, c->reply_len, "playing");
			return MM_OK;
		}
		if (queue_play_at(0, err, sizeof(err)) != 0) {
			snprintf(c->reply, c->reply_len, "%s", err);
			return MM_ERR;
		}
	} else {
		char uri[2048];
		join_args(c->argc, c->argv, 1, uri, sizeof(uri));
		if (queue_play_uri(uri, err, sizeof(err)) != 0) {
			snprintf(c->reply, c->reply_len, "%s", err);
			return MM_ERR;
		}
	}
	char cur[1024];
	player_current_uri(cur, sizeof(cur));
	snprintf(c->reply, c->reply_len, "playing %s", cur);
	return MM_OK;
}

static int step(struct mm_verb_ctx *c, int (*fn)(char *, size_t))
{
	char err[256];
	if (fn(err, sizeof(err)) != 0) {
		snprintf(c->reply, c->reply_len, "%s", err);
		return MM_ERR;
	}
	char cur[1024];
	player_current_uri(cur, sizeof(cur));
	snprintf(c->reply, c->reply_len, "playing %ld: %s", queue_cursor(), cur);
	return MM_OK;
}

static int v_next(void *priv, struct mm_verb_ctx *c) { (void)priv; return step(c, queue_next); }
static int v_prev(void *priv, struct mm_verb_ctx *c) { (void)priv; return step(c, queue_prev); }

static int v_queue(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	const char *sub = c->argc > 1 ? c->argv[1] : "list";
	long pos;
	char err[256];

	if (!strcmp(sub, "list")) {
		queue_list(c->reply, c->reply_len);
		if (!c->reply[0]) snprintf(c->reply, c->reply_len, "queue is empty\n");
		return MM_REPLY_RAW;
	}
	if (!strcmp(sub, "add")) {
		if (c->argc < 3) {
			snprintf(c->reply, c->reply_len, "usage: queue add <uri>...");
			return MM_ERR;
		}
		long first = -1, last = -1;
		for (int i = 2; i < c->argc; i++) {
			last = queue_add(c->argv[i]);
			if (last < 0) {
				snprintf(c->reply, c->reply_len, "queue full");
				return MM_ERR;
			}
			if (first < 0) first = last;
		}
		if (first == last) snprintf(c->reply, c->reply_len, "added at %ld", first);
		else snprintf(c->reply, c->reply_len, "added %ld-%ld", first, last);
		return MM_OK;
	}
	if (!strcmp(sub, "rm") && c->argc == 3 && parse_pos(c->argv[2], &pos) == 0) {
		if (queue_remove(pos) != 0) {
			snprintf(c->reply, c->reply_len, "no item %ld", pos);
			return MM_ERR;
		}
		return MM_OK;
	}
	if (!strcmp(sub, "clear") && c->argc == 2) {
		queue_clear();
		return MM_OK;
	}
	if (!strcmp(sub, "play") && c->argc == 3 && parse_pos(c->argv[2], &pos) == 0) {
		if (queue_play_at(pos, err, sizeof(err)) != 0) {
			snprintf(c->reply, c->reply_len, "%s", err);
			return MM_ERR;
		}
		snprintf(c->reply, c->reply_len, "playing %ld", pos);
		return MM_OK;
	}
	snprintf(c->reply, c->reply_len, "usage: queue [list | add <uri>... | rm <n> | clear | play <n>]");
	return MM_ERR;
}

int queue_init(void)
{
	int rc = 0;
	rc |= ipc_register("queue", "play", "[uri]\tplay a URI now (inserted after the current item), or resume/start the queue", v_play, NULL);
	rc |= ipc_register("queue", "next", "\tnext item in the queue", v_next, NULL);
	rc |= ipc_register("queue", "prev", "\tprevious item in the queue", v_prev, NULL);
	rc |= ipc_register("queue", "queue", "[list|add <uri>...|rm <n>|clear|play <n>]\tthe play queue", v_queue, NULL);
	return rc ? -1 : 0;
}

void queue_fini(void)
{
	ipc_unregister_owner("queue");
	queue_clear();
	pthread_mutex_lock(&Q.lock);
	free(Q.uri);
	Q.uri = NULL;
	Q.cap = 0;
	pthread_mutex_unlock(&Q.lock);
}

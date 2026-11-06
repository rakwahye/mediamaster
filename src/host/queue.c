/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file queue.c
 * @brief Play queue and cursor.
 *
 * Lock order: queue lock, then (inside player_*) the player lock. The
 * player never calls back into the queue, so this cannot invert. */

#include "queue.h"
#include "catalog.h"
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

/* Each entry is one allocation: the URI, a NUL, then its label, a NUL.
 * The label is "title\tartist\talbum\tduration", any field empty, or ""
 * when nothing is known; queue list -l prints it. */
static struct {
	pthread_mutex_t lock;
	char **uri;
	size_t n, cap;
	long cur;         /* 0-based index; -1 = none */
	int cur_removed;  /* the item at `cur` was deleted; cur now names its successor */
	long pre;         /* index handed to the player as the next item */
	int pre_valid;
	int pre_removed;  /* like cur_removed, for pre */
	unsigned long long gen; /* bumped on every change */
} Q = { .lock = PTHREAD_MUTEX_INITIALIZER, .cur = -1 };

/* The list changed. A preload the reader has not started on is stale:
 * drop it, and the player will ask again. One it already reads from
 * plays on, and `pre` keeps tracking it. */
static void edited(void)
{
	Q.gen++;
	if (Q.pre_valid && player_preload_state() == 2) return;
	player_drop_preload();
	Q.pre_valid = 0;
}

/* ------------------------------------------------------------------ storage (lock held) */

static const char *label_of(size_t i) { return Q.uri[i] + strlen(Q.uri[i]) + 1; }

static char *entry_new(const char *uri, const char *label)
{
	size_t ul = strlen(uri), ll = strlen(label);
	char *e = malloc(ul + ll + 2);
	if (!e) return NULL;
	memcpy(e, uri, ul + 1);
	memcpy(e + ul + 1, label, ll + 1);
	return e;
}

static int insert_at(size_t i, const char *uri, const char *label)
{
	if (Q.n >= QUEUE_MAX) return -1;
	if (Q.n == Q.cap) {
		size_t cap = Q.cap ? Q.cap * 2 : 64;
		char **v = realloc(Q.uri, cap * sizeof(*v));
		if (!v) return -1;
		Q.uri = v;
		Q.cap = cap;
	}
	char *d = entry_new(uri, label);
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
	Q.gen++;
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

long queue_add(const char *uri) { return queue_add_labeled(uri, ""); }

long queue_add_labeled(const char *uri, const char *label)
{
	pthread_mutex_lock(&Q.lock);
	long pos = insert_at(Q.n, uri, label ? label : "") == 0 ? (long)Q.n : -1;
	if (pos > 0) edited();
	pthread_mutex_unlock(&Q.lock);
	return pos;
}

/* Lock held. Take out [at, at + n), as if never inserted. */
static void remove_block(size_t at, size_t n)
{
	for (size_t i = 0; i < n; i++) free(Q.uri[at + i]);
	memmove(Q.uri + at, Q.uri + at + n, (Q.n - at - n) * sizeof(*Q.uri));
	Q.n -= n;
	if (Q.cur >= (long)(at + n)) Q.cur -= (long)n;
	if (Q.pre_valid && Q.pre >= (long)(at + n)) Q.pre -= (long)n;
}

int queue_play_list(char *const *uris, char *const *labels, size_t n, char *err, size_t errlen)
{
	if (!n) {
		snprintf(err, errlen, "nothing to play");
		return -1;
	}
	pthread_mutex_lock(&Q.lock);
	/* After the cursor; if the cursor's item was removed, at its slot. */
	size_t at = Q.cur < 0 ? Q.n : (size_t)(Q.cur + (Q.cur_removed ? 0 : 1));
	if (at > Q.n) at = Q.n;
	size_t in = 0;
	while (in < n && insert_at(at + in, uris[in], labels ? labels[in] : "") == 0) in++;
	int rc = -1;
	if (in < n) {
		snprintf(err, errlen, "queue full");
	} else {
		rc = play_index((long)at, err, errlen);
	}
	if (rc != 0) { /* not playable: leave the queue as it was */
		remove_block(at, in);
		edited();
	}
	pthread_mutex_unlock(&Q.lock);
	return rc;
}

int queue_play_uri(const char *uri, char *err, size_t errlen)
{
	char *one[1] = { (char *)uri };
	return queue_play_list(one, NULL, 1, err, errlen);
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

int queue_on_last(char *uri, size_t cap)
{
	pthread_mutex_lock(&Q.lock);
	int last = Q.cur >= 0 && !Q.cur_removed && (size_t)Q.cur + 1 == Q.n;
	if (last) snprintf(uri, cap, "%s", Q.uri[Q.cur]);
	pthread_mutex_unlock(&Q.lock);
	return last;
}

int queue_contains(const char *uri)
{
	pthread_mutex_lock(&Q.lock);
	int found = 0;
	for (size_t i = 0; i < Q.n && !found; i++) found = !strcmp(Q.uri[i], uri);
	pthread_mutex_unlock(&Q.lock);
	return found;
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
	Q.gen++;
}

static void on_finished_locked(void);

/* "key: value" from status-style tag lines into out ("" if absent). */
static void tag(const char *tags, const char *key, char *out, size_t cap)
{
	size_t kl = strlen(key);
	out[0] = '\0';
	for (const char *l = tags; *l; ) {
		const char *nl = strchr(l, '\n');
		size_t n = nl ? (size_t)(nl - l) : strlen(l);
		if (n > kl + 2 && !strncmp(l, key, kl) && l[kl] == ':' && l[kl + 1] == ' ') {
			snprintf(out, cap, "%.*s", (int)(n - kl - 2), l + kl + 2);
			return;
		}
		l += n + (nl ? 1 : 0);
	}
}

/* Lock held. Once the item at the cursor is playing and the player knows
 * its title, that becomes its label: what played is named in the list
 * even when it was queued as a bare URI. */
static void learn_label(void)
{
	static uint32_t item, metas;
	uint32_t i = player_item(), m = player_meta_count();
	if (i == item && m == metas) return;
	item = i;
	metas = m;
	if (Q.cur < 0 || Q.cur_removed || (size_t)Q.cur >= Q.n || !i) return;

	char uri[2048], tags[4096], src[64], t[256], a[256], al[256], dur[24] = "", label[1024];
	uint64_t ms;
	player_current(uri, sizeof(uri), tags, sizeof(tags), &ms, src, sizeof(src));
	if (strcmp(uri, Q.uri[Q.cur])) return;
	tag(tags, "title", t, sizeof(t));
	if (!t[0]) return;
	tag(tags, "artist", a, sizeof(a));
	tag(tags, "album", al, sizeof(al));
	if (ms) snprintf(dur, sizeof(dur), "%llu:%02llu", (unsigned long long)(ms + 500) / 60000,
	                 (unsigned long long)((ms + 500) / 1000 % 60));
	queue_label_make(label, sizeof(label), t, a, al, dur);
	if (!strcmp(label, label_of((size_t)Q.cur))) return;
	char *e = entry_new(uri, label);
	if (!e) return;
	free(Q.uri[Q.cur]);
	Q.uri[Q.cur] = e;
	Q.gen++; /* the list reads differently: watchers re-read it */
}

int queue_tick(void)
{
	int ev = player_tick();
	pthread_mutex_lock(&Q.lock);
	if (ev & PLAYER_EV_ADVANCED) on_advanced();
	if (ev & PLAYER_EV_FINISHED) on_finished_locked();
	if (player_wants_next()) preload_next();
	learn_label();
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

unsigned long long queue_generation(void)
{
	pthread_mutex_lock(&Q.lock);
	unsigned long long g = Q.gen;
	pthread_mutex_unlock(&Q.lock);
	return g;
}

/* Label fields, "-" for empty ones, as browse prints them. */
static int put_label(char *buf, size_t cap, const char *label)
{
	size_t off = 0;
	for (int f = 0; f < 4 && off < cap; f++) {
		const char *end = strchr(label, '\t');
		size_t n = end ? (size_t)(end - label) : strlen(label);
		int w = snprintf(buf + off, cap - off, "\t%.*s", n ? (int)n : 1, n ? label : "-");
		if (w < 0) return -1;
		off += (size_t)w;
		label = end ? end + 1 : label + n;
	}
	return (int)off;
}

static void list(char *buf, size_t cap, int labeled)
{
	size_t off = 0;
	if (cap) buf[0] = '\0';
	pthread_mutex_lock(&Q.lock);
	for (size_t i = 0; i < Q.n && off < cap; i++) {
		int mark = (long)i == Q.cur && !Q.cur_removed;
		int w = labeled ? snprintf(buf + off, cap - off, "%s%zu\t%s", mark ? ">" : "", i + 1, Q.uri[i])
		                : snprintf(buf + off, cap - off, "%s%zu  %s\n", mark ? "> " : "  ", i + 1, Q.uri[i]);
		if (w < 0) break;
		off += (size_t)w;
		if (!labeled || off >= cap) continue;
		w = put_label(buf + off, cap - off, label_of(i));
		if (w < 0) break;
		off += (size_t)w;
		if (off + 1 < cap) { buf[off++] = '\n'; buf[off] = '\0'; }
	}
	pthread_mutex_unlock(&Q.lock);
	if (off >= cap && cap > 4) memcpy(buf + cap - 5, "...\n", 5);
}

void queue_list(char *buf, size_t cap) { list(buf, cap, 0); }
void queue_list_long(char *buf, size_t cap) { list(buf, cap, 1); }

/* ------------------------------------------------------------------ labels */

static size_t put_clean(char *out, size_t cap, const char *s)
{
	size_t n = 0;
	if (!strcmp(s, "-")) s = ""; /* browse's mark for an empty field */
	for (; *s && n + 1 < cap; s++) out[n++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
	out[n] = '\0';
	return n;
}

void queue_label_make(char *out, size_t cap, const char *title, const char *artist, const char *album,
                      const char *duration)
{
	const char *f[4] = { title, artist, album, duration };
	size_t off = 0;
	if (!cap) return;
	out[0] = '\0';
	for (int i = 0; i < 4 && off + 1 < cap; i++) {
		if (i) out[off++] = '\t';
		off += put_clean(out + off, cap - off, f[i] ? f[i] : "");
	}
	out[off < cap ? off : cap - 1] = '\0';
	if (strspn(out, "\t") == strlen(out)) out[0] = '\0'; /* nothing known */
}

/* A browse or search line "kind\turi\ttitle\tartist\talbum\tduration"
 * (a frame word may hold tabs) into its fields, in place.
 * @returns the field count. */
static int split_line(char *line, char **f, int max)
{
	int n = 0;
	while (n < max) {
		f[n++] = line;
		char *t = strchr(line, '\t');
		if (!t) break;
		*t = '\0';
		line = t + 1;
	}
	for (int i = n; i < max; i++) f[i] = (char *)"";
	return n;
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
		char uri[2048], **v;
		join_args(c->argc, c->argv, 1, uri, sizeof(uri));
		/* Words, not a URI or a path: the first track a search finds. */
		if (catalog_is_query(uri)) {
			char hit[2048];
			if (catalog_first_hit(uri, hit, sizeof(hit), err, sizeof(err)) != 0) {
				snprintf(c->reply, c->reply_len, "%s", err);
				return MM_ERR;
			}
			snprintf(uri, sizeof(uri), "%s", hit);
		}
		long n = catalog_tracks(uri, &v, QUEUE_MAX, err, sizeof(err));
		char **lab = n > 0 ? malloc((size_t)n * sizeof(*lab)) : NULL;
		for (long k = 0; lab && k < n; k++) lab[k] = (char *)catalog_label(v[k]);
		int rc = n < 0 ? -1 : queue_play_list(v, lab, (size_t)n, err, sizeof(err));
		free(lab);
		if (n >= 0) catalog_free(v, n);
		if (rc != 0) {
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

	if (!strcmp(sub, "list") && (c->argc < 3 || (c->argc == 3 && !strcmp(c->argv[2], "-l")))) {
		if (c->argc == 3) queue_list_long(c->reply, c->reply_len);
		else queue_list(c->reply, c->reply_len);
		if (!c->reply[0]) snprintf(c->reply, c->reply_len, "queue is empty\n");
		return MM_REPLY_RAW;
	}
	if (!strcmp(sub, "add")) {
		if (c->argc < 3) {
			snprintf(c->reply, c->reply_len, "usage: queue add <uri>...");
			return MM_ERR;
		}
		/* Each argument is a track, or a list that stands for its tracks.
		 * An argument with tabs is a line as browse and search print
		 * them: its URI is queued, a track's fields become its label. */
		long first = -1, last = -1;
		for (int i = 2; i < c->argc; i++) {
			char **v, *f[6], line[4096], label[1024];
			const char *arg = c->argv[i];
			label[0] = '\0';
			if (strchr(arg, '\t')) {
				snprintf(line, sizeof(line), "%s", arg);
				if (split_line(line, f, 6) < 2 || !f[1][0]) {
					snprintf(c->reply, c->reply_len, "not a browse line: %.64s", arg);
					return MM_ERR;
				}
				arg = f[1];
				if (!strcmp(f[0], "track")) queue_label_make(label, sizeof(label), f[2], f[3], f[4], f[5]);
			}
			long n = catalog_tracks(arg, &v, QUEUE_MAX, err, sizeof(err));
			if (n < 0) {
				snprintf(c->reply, c->reply_len, "%s", err);
				return MM_ERR;
			}
			for (long k = 0; k < n && last != -2; k++) {
				/* What the line said, else what the catalog gave. */
				long at = queue_add_labeled(v[k], n == 1 && label[0] ? label : catalog_label(v[k]));
				if (at < 0) last = -2;
				else if (first < 0) first = last = at;
				else last = at;
			}
			catalog_free(v, n);
			if (last == -2) {
				snprintf(c->reply, c->reply_len, "queue full");
				return MM_ERR;
			}
		}
		if (first < 0) snprintf(c->reply, c->reply_len, "nothing to add");
		else if (first == last) snprintf(c->reply, c->reply_len, "added at %ld", first);
		else snprintf(c->reply, c->reply_len, "added %ld-%ld", first, last);
		return first < 0 ? MM_ERR : MM_OK;
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
	snprintf(c->reply, c->reply_len, "usage: queue [list [-l] | add <uri|line>... | rm <n> | clear | play <n>]");
	return MM_ERR;
}

int queue_init(void)
{
	int rc = 0;
	rc |= ipc_register("queue", "play", "[uri|words]\tplay a URI, or the first search hit, now (after the current item); or resume/start the queue", v_play, NULL);
	rc |= ipc_register("queue", "next", "\tnext item in the queue", v_next, NULL);
	rc |= ipc_register("queue", "prev", "\tprevious item in the queue", v_prev, NULL);
	rc |= ipc_register("queue", "queue", "[list [-l]|add <uri|line>...|rm <n>|clear|play <n>]\tthe play queue; -l: n uri title artist album duration, tab-separated, > at the cursor", v_queue, NULL);
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

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file catalog.c
 * @brief `browse`, and lists expanded into tracks for the queue.
 *
 * Expansion collects a list's items before going into any nested list,
 * so a plugin is never re-entered from inside its own `list`. Nesting
 * stops at MAX_DEPTH, which also ends any cycle. */

#include "catalog.h"
#include "config.h"
#include "ipc.h"
#include "loader.h"
#include "log.h"
#include "queue.h"
#include "uri.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_DEPTH 8
#define TAG "catalog"

int catalog_list(const char *uri, uint32_t flags, mm_item_fn emit, void *ctx, char *err, size_t errlen)
{
	char scheme[32];
	const struct mm_plugin *p;
	void *priv;

	err[0] = '\0';
	uri_scheme(uri, scheme, sizeof(scheme));
	if (loader_find_catalog(scheme, &p, &priv) != 0) {
		snprintf(err, errlen, "nothing lists '%s:'", scheme);
		return -1;
	}
	int rc = p->catalog->list(priv, uri, flags, emit, ctx, err, errlen);
	if (rc == MM_NOT_A_LIST || rc == 0) return rc;
	if (!err[0]) snprintf(err, errlen, "%s could not list %s", p->name, uri);
	return -1;
}

/* ------------------------------------------------------------------ expansion */

struct vec { char **v; long n, cap; };

static int vec_push(struct vec *a, const char *s)
{
	if (a->n == a->cap) {
		long cap = a->cap ? a->cap * 2 : 64;
		char **v = realloc(a->v, (size_t)cap * sizeof(*v));
		if (!v) return -1;
		a->v = v;
		a->cap = cap;
	}
	char *d = strdup(s);
	if (!d) return -1;
	a->v[a->n++] = d;
	return 0;
}

/* A URI with its label after the NUL, as one allocation. */
static int vec_push_entry(struct vec *a, const char *uri, const char *label)
{
	if (vec_push(a, "") != 0) return -1;
	size_t ul = strlen(uri), ll = strlen(label);
	char *e = malloc(ul + ll + 2);
	if (!e) return -1;
	memcpy(e, uri, ul + 1);
	memcpy(e + ul + 1, label, ll + 1);
	free(a->v[a->n - 1]);
	a->v[a->n - 1] = e;
	return 0;
}

const char *catalog_label(const char *entry)
{
	return entry + strlen(entry) + 1;
}

void catalog_item_label(const struct mm_item *it, char *out, size_t cap)
{
	char dur[24] = "";
	if (it->duration_ms) {
		unsigned long long s = (unsigned long long)((it->duration_ms + 500) / 1000);
		snprintf(dur, sizeof(dur), "%llu:%02llu", s / 60, s % 60);
	}
	queue_label_make(out, cap, it->title ? it->title : "", it->artist ? it->artist : "",
	                 it->album ? it->album : "", dur);
}

void catalog_free(char **v, long n)
{
	for (long i = 0; i < n; i++) free(v[i]);
	free(v);
}

/* One list's items, in order: tracks and lists both, lists marked. */
struct gather { struct vec items; struct vec kinds; int oom; };

static int gather(void *ctx, const struct mm_item *it)
{
	struct gather *g = ctx;
	char label[1024] = "";
	if (!it->uri || (it->kind != MM_ITEM_TRACK && it->kind != MM_ITEM_LIST)) return 0;
	if (it->kind == MM_ITEM_TRACK) catalog_item_label(it, label, sizeof(label));
	if (vec_push_entry(&g->items, it->uri, label) != 0 ||
	    vec_push(&g->kinds, it->kind == MM_ITEM_LIST ? "l" : "t") != 0) {
		g->oom = 1;
		return 1;
	}
	return 0;
}

static int expand(const char *uri, int depth, struct vec *out, long max, char *err, size_t errlen)
{
	struct gather g = { { NULL, 0, 0 }, { NULL, 0, 0 }, 0 };
	int rc = catalog_list(uri, MM_LIST_URIS, gather, &g, err, errlen);
	if (rc == MM_NOT_A_LIST) {
		rc = vec_push_entry(out, uri, "");
	} else if (rc == 0 && g.oom) {
		snprintf(err, errlen, "out of memory");
		rc = -1;
	} else if (rc == 0) {
		for (long i = 0; i < g.items.n && out->n < max && rc == 0; i++) {
			if (g.kinds.v[i][0] == 't') rc = vec_push_entry(out, g.items.v[i], catalog_label(g.items.v[i]));
			else if (depth < MAX_DEPTH) {
				/* A nested list that fails is skipped, not fatal. */
				char sub[256];
				if (expand(g.items.v[i], depth + 1, out, max, sub, sizeof(sub)) != 0)
					LOGW(TAG, "skipped %s: %s", g.items.v[i], sub);
			}
		}
		if (rc != 0) snprintf(err, errlen, "out of memory");
	}
	catalog_free(g.items.v, g.items.n);
	catalog_free(g.kinds.v, g.kinds.n);
	return rc;
}

long catalog_tracks(const char *uri, char ***out, long max, char *err, size_t errlen)
{
	struct vec v = { NULL, 0, 0 };
	char scheme[32];
	const struct mm_plugin *p;
	void *priv;

	err[0] = '\0';
	uri_scheme(uri, scheme, sizeof(scheme));
	if (loader_find_catalog(scheme, &p, &priv) != 0) {
		if (vec_push_entry(&v, uri, "") != 0) return -1; /* nothing lists it: one track */
	} else if (expand(uri, 0, &v, max, err, errlen) != 0) {
		catalog_free(v.v, v.n);
		return -1;
	}
	while (v.n > max) free(v.v[--v.n]);
	*out = v.v;
	return v.n;
}

/* ------------------------------------------------------------------ browse */

struct out { char *buf; size_t cap, len; long n; int full; };

/* Append s with tabs and line breaks made spaces; "-" when empty. */
static void put_field(struct out *o, const char *s, int last)
{
	if (!s || !*s) s = "-";
	for (; *s && o->len + 2 < o->cap; s++)
		o->buf[o->len++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
	if (o->len + 1 < o->cap) o->buf[o->len++] = last ? '\n' : '\t';
	o->buf[o->len] = '\0';
}

static int print_item(void *ctx, const struct mm_item *it)
{
	struct out *o = ctx;
	char dur[24] = "";
	if (!it->uri) return 0;
	if (o->len + 4096 > o->cap) { /* keep whole lines */
		o->full = 1;
		return 1;
	}
	if (it->duration_ms) {
		unsigned long long s = (unsigned long long)((it->duration_ms + 500) / 1000);
		snprintf(dur, sizeof(dur), "%llu:%02llu", s / 60, s % 60);
	}
	put_field(o, it->kind == MM_ITEM_LIST ? "list" : "track", 0);
	put_field(o, it->uri, 0);
	put_field(o, it->title, 0);
	put_field(o, it->artist, 0);
	put_field(o, it->album, 0);
	put_field(o, dur, 1);
	o->n++;
	return 0;
}

static int v_browse(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	if (c->argc != 2) {
		snprintf(c->reply, c->reply_len,
		         "usage: browse <uri>   (a top level: <scheme>:, e.g. lib: or file:)\n"
		         "lines: kind uri title artist album duration, tab-separated");
		return MM_ERR;
	}
	char err[512];
	struct out o = { c->reply, c->reply_len, 0, 0, 0 };
	c->reply[0] = '\0';
	int rc = catalog_list(c->argv[1], 0, print_item, &o, err, sizeof(err));
	if (rc == MM_NOT_A_LIST) {
		snprintf(c->reply, c->reply_len, "%s is a track, not a list", c->argv[1]);
		return MM_ERR;
	}
	if (rc != 0) {
		snprintf(c->reply, c->reply_len, "%s", err);
		return MM_ERR;
	}
	if (o.full && o.len + 64 < o.cap)
		o.len += (size_t)snprintf(o.buf + o.len, o.cap - o.len, "... more than %ld items, rest not shown\n", o.n);
	if (!o.n) snprintf(c->reply, c->reply_len, "empty\n");
	return MM_REPLY_RAW;
}

/* ------------------------------------------------------------------ search
 *
 * Every catalog is asked for "<scheme>:search:<words>", in load order; a
 * catalog that cannot search answers with an error or MM_NOT_A_LIST and
 * is passed over. Each gives at most catalog.search_max results. */

int catalog_is_query(const char *text)
{
	char scheme[32];
	const struct mm_plugin *p;
	void *priv;
	if (!text[0] || text[0] == '/') return 0;
	uri_scheme(text, scheme, sizeof(scheme));
	if (!strcmp(scheme, "file")) return strncmp(text, "file:", 5) != 0; /* bare words */
	return loader_find_source(scheme, &p, &priv) != 0 && loader_find_catalog(scheme, &p, &priv) != 0;
}

static long search_max(void)
{
	const char *v = config_get("catalog.search_max");
	long n = v ? strtol(v, NULL, 10) : 0;
	return n > 0 ? n : 20;
}

struct search {
	const char *query;
	mm_item_fn emit;
	void *ctx;
	long left, max;
	int stop, sources;
};

static int counted(void *ctx, const struct mm_item *it)
{
	struct search *s = ctx;
	if (s->left <= 0) return 1;
	s->left--;
	s->stop = s->emit(s->ctx, it) != 0;
	return s->stop || s->left <= 0;
}

static void search_one(void *ctx, const char *scheme, const char *name)
{
	struct search *s = ctx;
	char uri[1024], err[256];
	if (s->stop) return;
	struct timespec t0, t1;
	snprintf(uri, sizeof(uri), "%s:search:%s", scheme, s->query);
	s->left = s->max;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int rc = catalog_list(uri, 0, counted, s, err, sizeof(err));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
	if (rc == 0) {
		s->sources++;
		LOGD(TAG, "search %s: %ld results in %ld ms", name, s->max - s->left, ms);
	} else if (rc != MM_NOT_A_LIST) {
		LOGD(TAG, "search %s: none (%s) in %ld ms", name, err, ms);
	}
}

static int v_search(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	char q[1024];
	size_t off = 0;
	q[0] = '\0';
	for (int i = 1; i < c->argc && off < sizeof(q); i++)
		off += (size_t)snprintf(q + off, sizeof(q) - off, "%s%s", i > 1 ? " " : "", c->argv[i]);
	if (!q[0]) {
		snprintf(c->reply, c->reply_len, "usage: search <words>");
		return MM_ERR;
	}
	struct out o = { c->reply, c->reply_len, 0, 0, 0 };
	struct search s = { q, print_item, &o, 0, search_max(), 0, 0 };
	c->reply[0] = '\0';
	loader_each_catalog(search_one, &s);
	if (!o.n) snprintf(c->reply, c->reply_len, "no results\n");
	return MM_REPLY_RAW;
}

struct first { char *uri; size_t cap; int found; };

static int take_first(void *ctx, const struct mm_item *it)
{
	struct first *f = ctx;
	if (it->kind != MM_ITEM_TRACK || !it->uri) return 0; /* the first track, not a list */
	snprintf(f->uri, f->cap, "%s", it->uri);
	f->found = 1;
	return 1;
}

int catalog_first_hit(const char *query, char *uri, size_t cap, char *err, size_t errlen)
{
	struct first f = { uri, cap, 0 };
	struct search s = { query, take_first, &f, 0, search_max(), 0, 0 };
	loader_each_catalog(search_one, &s);
	if (f.found) return 0;
	snprintf(err, errlen, "nothing found for '%s'", query);
	return -1;
}

int catalog_init(void)
{
	int rc = ipc_register("catalog", "browse",
	                      "<uri>\twhat is inside a playlist, album, folder (<scheme>: for the top)",
	                      v_browse, NULL);
	rc |= ipc_register("catalog", "search", "<words>\tsearch every source; play <words> plays the first hit",
	                   v_search, NULL);
	return rc;
}

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file catalog.c
 * @brief `browse`, and lists expanded into tracks for the queue.
 *
 * Expansion collects a list's items before going into any nested list,
 * so a plugin is never re-entered from inside its own `list`. Nesting
 * stops at MAX_DEPTH, which also ends any cycle. */

#include "catalog.h"
#include "ipc.h"
#include "loader.h"
#include "log.h"
#include "uri.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	if (!it->uri || (it->kind != MM_ITEM_TRACK && it->kind != MM_ITEM_LIST)) return 0;
	if (vec_push(&g->items, it->uri) != 0 || vec_push(&g->kinds, it->kind == MM_ITEM_LIST ? "l" : "t") != 0) {
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
		rc = vec_push(out, uri);
	} else if (rc == 0 && g.oom) {
		snprintf(err, errlen, "out of memory");
		rc = -1;
	} else if (rc == 0) {
		for (long i = 0; i < g.items.n && out->n < max && rc == 0; i++) {
			if (g.kinds.v[i][0] == 't') rc = vec_push(out, g.items.v[i]);
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
		if (vec_push(&v, uri) != 0) return -1; /* nothing lists it: one track */
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

int catalog_init(void)
{
	return ipc_register("catalog", "browse",
	                    "<uri>\twhat is inside a playlist, album, folder (<scheme>: for the top)",
	                    v_browse, NULL);
}

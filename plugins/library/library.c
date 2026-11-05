/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file library.c
 * @brief Your music folder as a library: artists, albums and search by
 * tags, from an index kept up to date in the background (index.c).
 *
 *   lib:                          Artists, Albums
 *   lib:artists                   every album artist (or artist)
 *   lib:artist:<artist>           that artist's albums
 *   lib:albums                    every album
 *   lib:album:<artist>/<album>    its tracks, by disc and track number
 *   lib:search:<words>            tracks whose tags or path hold every word
 *
 * Names in URIs are percent-encoded ('/', '%' and control characters).
 * A file without tags is filed under "(no artist)" with its folder as the
 * album, so an untagged rip still browses as the folder it came in.
 * Tracks are their file paths, so the ffmpeg source plays them.
 *
 * Config:
 *   library.music_dir = ~/Music
 *   library.index     = $XDG_CACHE_HOME/mediamaster/library.idx, or "off"
 *   library.ffprobe   = ffprobe
 *   library.probe_jobs = 4      ffprobes at once while tagging (1..16) */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* versionsort, strverscmp, pipe2, d_type */
#endif
#include "mm_plugin.h"
#include "index.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NO_ARTIST "(no artist)"
#define NO_ALBUM "(no album)"

struct lib {
	const struct mm_host *h;
	pthread_mutex_t lock; /* idx: reload replaces it while a list may run */
	struct libidx *idx;
	char root[4096], saved[4096], ffprobe[1024];
};

/* ------------------------------------------------------------------ setup */

static void cfg(const struct mm_host *h, const char *key, const char *def, char *out, size_t cap)
{
	const char *v = h->config_get(h, key);
	snprintf(out, cap, "%s", v && *v ? v : def);
}

static void make_parents(const char *file)
{
	char d[4096];
	snprintf(d, sizeof(d), "%s", file);
	for (char *s = d + 1; *s; s++) {
		if (*s != '/') continue;
		*s = '\0';
		mkdir(d, 0700);
		*s = '/';
	}
}

static void setup(struct lib *l)
{
	const struct mm_host *h = l->h;
	char root[4096], saved[4096], ffprobe[1024], def[4096];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CACHE_HOME");

	snprintf(def, sizeof(def), "%s/Music", home ? home : "");
	cfg(h, "music_dir", def, root, sizeof(root));
	if (xdg && *xdg) snprintf(def, sizeof(def), "%s/mediamaster/library.idx", xdg);
	else snprintf(def, sizeof(def), "%s/.cache/mediamaster/library.idx", home ? home : "");
	cfg(h, "index", def, saved, sizeof(saved));
	cfg(h, "ffprobe", "ffprobe", ffprobe, sizeof(ffprobe));
	int off = !strcmp(saved, "off");
	const char *jv = h->config_get(h, "probe_jobs");
	int jobs = jv && *jv ? atoi(jv) : 4;

	pthread_mutex_lock(&l->lock);
	if (l->idx && !off && !strcmp(root, l->root) && !strcmp(saved, l->saved) && !strcmp(ffprobe, l->ffprobe)) {
		libidx_rescan(l->idx); /* the same library: bring it up to date */
	} else {
		libidx_close(l->idx);
		l->idx = NULL;
		snprintf(l->root, sizeof(l->root), "%s", root);
		snprintf(l->saved, sizeof(l->saved), "%s", saved);
		snprintf(l->ffprobe, sizeof(l->ffprobe), "%s", ffprobe);
		if (!off) {
			make_parents(saved);
			l->idx = libidx_open(h, root, saved, ffprobe);
		}
	}
	if (l->idx) libidx_probe_jobs(l->idx, jobs);
	pthread_mutex_unlock(&l->lock);
}

/* ------------------------------------------------------------------ names */

static void enc(const char *in, char *out, size_t cap)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
		if (*p == '/' || *p == '%' || *p < 0x20) {
			out[o++] = '%';
			out[o++] = hex[*p >> 4];
			out[o++] = hex[*p & 15];
		} else {
			out[o++] = (char)*p;
		}
	}
	out[o] = '\0';
}

static void dec(const char *in, size_t n, char *out, size_t cap)
{
	size_t o = 0;
	for (size_t i = 0; i < n && o + 1 < cap; i++) {
		if (in[i] == '%' && i + 2 < n) {
			char h[3] = { in[i + 1], in[i + 2], 0 };
			char *e;
			long c = strtol(h, &e, 16);
			if (!*e) {
				out[o++] = (char)c;
				i += 2;
				continue;
			}
		}
		out[o++] = in[i];
	}
	out[o] = '\0';
}

static int cmp_ci(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : (unsigned char)*a;
		int y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : (unsigned char)*b;
		if (x != y) return x - y;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

/* ------------------------------------------------------------------ rows */

/* One track, copied out of the index so it can be sorted and emitted
 * after the index's lock is let go. */
struct row {
	char *path, *title, *artist, *group, *album;
	int disc, track;
	uint64_t ms;
};

struct rows {
	struct row *v;
	size_t n, cap;
	int oom;
	/* What to keep: */
	const char *group, *album;   /* NULL: any */
	char words[16][128];         /* all must appear; nwords 0: no filter */
	int nwords;
};

static void lower(const char *in, char *out, size_t cap)
{
	size_t i = 0;
	for (; in[i] && i + 1 < cap; i++) out[i] = (in[i] >= 'A' && in[i] <= 'Z') ? (char)(in[i] + 32) : in[i];
	out[i] = '\0';
}

/* The track's group artist and album: by tags, or the folder. */
static void place(const struct libidx_track *t, char *group, size_t gc, char *album, size_t ac)
{
	libidx_tag(t->tags, "albumartist", group, gc);
	if (!group[0]) libidx_tag(t->tags, "artist", group, gc);
	libidx_tag(t->tags, "album", album, ac);
	if (!group[0] && !album[0]) {
		const char *slash = strrchr(t->rel, '/');
		snprintf(album, ac, "%.*s", slash ? (int)(slash - t->rel) : 0, t->rel);
	}
	if (!group[0]) snprintf(group, gc, "%s", NO_ARTIST);
	if (!album[0]) snprintf(album, ac, "%s", NO_ALBUM);
}

static int take(void *ctx, const struct libidx_track *t)
{
	struct rows *r = ctx;
	char group[512], album[512], title[512], artist[512], num[16];
	place(t, group, sizeof(group), album, sizeof(album));
	if (r->group && strcmp(r->group, group)) return 0;
	if (r->album && strcmp(r->album, album)) return 0;
	if (r->nwords) {
		char hay[12288], low[12288];
		snprintf(hay, sizeof(hay), "%s\n%s", t->rel, t->tags);
		lower(hay, low, sizeof(low));
		for (int i = 0; i < r->nwords; i++)
			if (!strstr(low, r->words[i])) return 0;
	}
	if (r->n == r->cap) {
		size_t cap = r->cap ? r->cap * 2 : 256;
		struct row *v = realloc(r->v, cap * sizeof(*v));
		if (!v) {
			r->oom = 1;
			return 1;
		}
		r->v = v;
		r->cap = cap;
	}
	libidx_tag(t->tags, "title", title, sizeof(title));
	libidx_tag(t->tags, "artist", artist, sizeof(artist));
	struct row *w = &r->v[r->n];
	memset(w, 0, sizeof(*w));
	w->path = strdup(t->path);
	w->title = strdup(title[0] ? title : t->name);
	w->artist = artist[0] ? strdup(artist) : NULL;
	w->group = strdup(group);
	w->album = strdup(album);
	libidx_tag(t->tags, "discnumber", num, sizeof(num));
	w->disc = atoi(num);
	libidx_tag(t->tags, "tracknumber", num, sizeof(num));
	w->track = atoi(num);
	w->ms = t->duration_ms;
	r->n++;
	if (!w->path || !w->title || !w->group || !w->album) r->oom = 1;
	return r->oom;
}

static void rows_free(struct rows *r)
{
	for (size_t i = 0; i < r->n; i++) {
		free(r->v[i].path);
		free(r->v[i].title);
		free(r->v[i].artist);
		free(r->v[i].group);
		free(r->v[i].album);
	}
	free(r->v);
}

static int gather(struct lib *l, struct rows *r, char *err, size_t errlen)
{
	pthread_mutex_lock(&l->lock);
	if (l->idx) libidx_each(l->idx, take, r);
	int on = l->idx != NULL;
	pthread_mutex_unlock(&l->lock);
	if (!on) {
		snprintf(err, errlen, "the library is off (library.index = off)");
		return -1;
	}
	if (r->oom) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	return 0;
}

static int by_group(const void *a, const void *b)
{
	const struct row *x = a, *y = b;
	int c = cmp_ci(x->group, y->group);
	return c ? c : cmp_ci(x->album, y->album);
}

static int by_album(const void *a, const void *b)
{
	const struct row *x = a, *y = b;
	int c = cmp_ci(x->album, y->album);
	return c ? c : cmp_ci(x->group, y->group);
}

static int by_position(const void *a, const void *b)
{
	const struct row *x = a, *y = b;
	if (x->disc != y->disc) return x->disc - y->disc;
	if (x->track != y->track) return x->track - y->track;
	return strverscmp(x->path, y->path);
}

static int emit_track(const struct row *w, mm_item_fn emit, void *ctx)
{
	struct mm_item it = { MM_ITEM_TRACK, w->path, w->title, w->artist ? w->artist : w->group,
	                      strcmp(w->album, NO_ALBUM) ? w->album : NULL, w->ms };
	return emit(ctx, &it);
}

static int emit_album(const struct row *w, mm_item_fn emit, void *ctx)
{
	char uri[2300], a[1100], b[1100];
	enc(w->group, a, sizeof(a));
	enc(w->album, b, sizeof(b));
	snprintf(uri, sizeof(uri), "lib:album:%s/%s", a, b);
	struct mm_item it = { MM_ITEM_LIST, uri, w->album, w->group, NULL, 0 };
	return emit(ctx, &it);
}

/* ------------------------------------------------------------------ catalog */

static int lib_list(void *priv, const char *uri, uint32_t flags, mm_item_fn emit, void *ctx, char *err, size_t errlen)
{
	struct lib *l = priv;
	struct rows r;
	char a[1024], b[1024];
	int rc = 0;
	(void)flags; /* everything is in memory: titles cost nothing */
	memset(&r, 0, sizeof(r));

	if (!strcmp(uri, "lib:")) {
		struct mm_item top[] = {
			{ MM_ITEM_LIST, "lib:artists", "Artists", NULL, NULL, 0 },
			{ MM_ITEM_LIST, "lib:albums", "Albums", NULL, NULL, 0 },
		};
		for (size_t i = 0; i < 2; i++)
			if (emit(ctx, &top[i])) break;
		return 0;
	}
	if (!strcmp(uri, "lib:artists")) {
		if (gather(l, &r, err, errlen) != 0) goto out;
		if (r.n) qsort(r.v, r.n, sizeof(*r.v), by_group);
		for (size_t i = 0; i < r.n; i++) {
			if (i && !strcmp(r.v[i].group, r.v[i - 1].group)) continue;
			char u[1200];
			enc(r.v[i].group, a, sizeof(a));
			snprintf(u, sizeof(u), "lib:artist:%s", a);
			struct mm_item it = { MM_ITEM_LIST, u, r.v[i].group, NULL, NULL, 0 };
			if (emit(ctx, &it)) break;
		}
	} else if (!strncmp(uri, "lib:artist:", 11)) {
		dec(uri + 11, strlen(uri + 11), a, sizeof(a));
		r.group = a;
		if (gather(l, &r, err, errlen) != 0) goto out;
		if (r.n) qsort(r.v, r.n, sizeof(*r.v), by_album);
		for (size_t i = 0; i < r.n; i++)
			if ((!i || strcmp(r.v[i].album, r.v[i - 1].album)) && emit_album(&r.v[i], emit, ctx)) break;
	} else if (!strcmp(uri, "lib:albums")) {
		if (gather(l, &r, err, errlen) != 0) goto out;
		if (r.n) qsort(r.v, r.n, sizeof(*r.v), by_album);
		for (size_t i = 0; i < r.n; i++) {
			int same = i && !strcmp(r.v[i].album, r.v[i - 1].album) && !strcmp(r.v[i].group, r.v[i - 1].group);
			if (!same && emit_album(&r.v[i], emit, ctx)) break;
		}
	} else if (!strncmp(uri, "lib:album:", 10)) {
		const char *s = uri + 10, *slash = strchr(s, '/');
		if (!slash) {
			snprintf(err, errlen, "not an album: %s", uri);
			return -1;
		}
		dec(s, (size_t)(slash - s), a, sizeof(a));
		dec(slash + 1, strlen(slash + 1), b, sizeof(b));
		r.group = a;
		r.album = b;
		if (gather(l, &r, err, errlen) != 0) goto out;
		if (r.n) qsort(r.v, r.n, sizeof(*r.v), by_position);
		for (size_t i = 0; i < r.n; i++)
			if (emit_track(&r.v[i], emit, ctx)) break;
	} else if (!strncmp(uri, "lib:search:", 11)) {
		char low[1024], *save = NULL;
		lower(uri + 11, low, sizeof(low));
		for (char *w = strtok_r(low, " \t", &save); w && r.nwords < 16; w = strtok_r(NULL, " \t", &save))
			snprintf(r.words[r.nwords++], sizeof(r.words[0]), "%s", w);
		if (!r.nwords) return 0;
		if (gather(l, &r, err, errlen) != 0) goto out;
		for (size_t i = 0; i < r.n; i++)
			if (emit_track(&r.v[i], emit, ctx)) break;
	} else {
		snprintf(err, errlen, "not a library list: %s", uri);
		return -1;
	}
	goto done;
out:
	rc = -1;
done:
	rows_free(&r);
	return rc;
}

/* ------------------------------------------------------------------ verb */

static int v_library(void *priv, struct mm_verb_ctx *c)
{
	struct lib *l = priv;
	struct libidx_stats st;
	if (c->argc > 1 && !strcmp(c->argv[1], "rescan")) {
		pthread_mutex_lock(&l->lock);
		if (l->idx) libidx_rescan(l->idx);
		pthread_mutex_unlock(&l->lock);
	} else if (c->argc > 1) {
		snprintf(c->reply, c->reply_len, "usage: library [rescan]");
		return MM_ERR;
	}
	pthread_mutex_lock(&l->lock);
	if (!l->idx) {
		pthread_mutex_unlock(&l->lock);
		snprintf(c->reply, c->reply_len, "off");
		return MM_OK;
	}
	libidx_stats(l->idx, &st);
	snprintf(c->reply, c->reply_len, "%s: %ld files, %ld with tags, %ld folders%s", l->root, st.files, st.probed,
	         st.dirs, st.scanning ? "; updating" : "");
	pthread_mutex_unlock(&l->lock);
	return MM_OK;
}

/* ------------------------------------------------------------------ plugin */

static int lib_init(const struct mm_host *h, void **priv)
{
	struct lib *l = calloc(1, sizeof(*l));
	if (!l) return -1;
	l->h = h;
	pthread_mutex_init(&l->lock, NULL);
	setup(l);
	if (h->register_verb(h, "library", "[rescan]\tthe music library: files, tags, scan state", v_library, l) != 0) {
		libidx_close(l->idx);
		pthread_mutex_destroy(&l->lock);
		free(l);
		return -1;
	}
	*priv = l;
	return 0;
}

static void lib_reload(void *priv)
{
	setup(priv);
}

static void lib_fini(void *priv)
{
	struct lib *l = priv;
	libidx_close(l->idx);
	pthread_mutex_destroy(&l->lock);
	free(l);
}

static const struct mm_catalog_ops s_catalog = { .list = lib_list };

static const struct mm_plugin s_plugin = {
	.abi = MM_ABI,
	.attrs = MM_ATTR_SPAWNS,
	.name = "library",
	.version = "0.1.0",
	.description = "your music folder, by artist, album and tags",
	.schemes = "lib",
	.init = lib_init,
	.fini = lib_fini,
	.reload = lib_reload,
	.catalog = &s_catalog,
};

MM_PLUGIN_DEFINE(library)
{
	return &s_plugin;
}

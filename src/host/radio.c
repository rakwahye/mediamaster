/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file radio.c
 * @brief Radio: the queue does not run dry.
 *
 * Once the item playing is the queue's last, radio providers (catalogs
 * with a radio op) are asked for more like it, on a thread of its own,
 * in the order radio.order gives; the first that names tracks wins, and
 * they are queued, the seed and anything already queued left out. That
 * happens a whole track ahead, so the preload carries straight on. Each
 * item seeds radio at most once.
 *
 * The seed is handed over as an item with the tags the player knows, so
 * a provider can place a seed from another source by what it is.
 *
 * Config:
 *   radio        = on | off   (also the `radio [on|off]` verb)
 *   radio.order  = same, *    providers by plugin name; "same" is the
 *                             seed's own source, "*" every other one
 *   radio.batch  = 25         tracks added at a time */

#include "radio.h"
#include "catalog.h"
#include "config.h"
#include "ipc.h"
#include "loader.h"
#include "log.h"
#include "player.h"
#include "queue.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "radio"
#define MAX_URIS 200

static struct {
	pthread_t th;
	int th_on;
	atomic_int done;    /* the fetch thread has finished */
	atomic_int off;     /* set by the verb; config read when unset */
	atomic_int verb;    /* the verb has spoken: it wins over config */
	uint32_t seeded;    /* item that last started a fetch */
	/* The seed, captured when the fetch starts. */
	char seed[2048], tags[2048], source[64];
	char title[512], artist[1024], album[512];
	uint64_t duration_ms;
} R;

static int enabled(void)
{
	if (atomic_load(&R.verb)) return !atomic_load(&R.off);
	const char *v = config_get("radio");
	return !(v && (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no")));
}

static long batch(void)
{
	const char *v = config_get("radio.batch");
	long n = v ? strtol(v, NULL, 10) : 0;
	return n > 0 && n <= MAX_URIS ? n : 25;
}

struct got { char *uri[MAX_URIS]; int n; };

static int gather(void *ctx, const struct mm_item *it)
{
	struct got *g = ctx;
	if (it->kind != MM_ITEM_TRACK || !it->uri || g->n == MAX_URIS) return g->n == MAX_URIS;
	g->uri[g->n] = strdup(it->uri);
	if (g->uri[g->n]) g->n++;
	return 0;
}

/* tags "artist: A\nartist: B\n" → "A, B"; the first value for others. */
static void tag(const char *tags, const char *key, int all, char *out, size_t cap)
{
	size_t kl = strlen(key), o = 0;
	out[0] = '\0';
	for (const char *p = tags; *p; ) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n > kl + 2 && !strncmp(p, key, kl) && p[kl] == ':' && p[kl + 1] == ' ') {
			int w = snprintf(out + o, cap - o, "%s%.*s", o ? ", " : "", (int)(n - kl - 2), p + kl + 2);
			if (w < 0 || (size_t)w >= cap - o) break;
			o += (size_t)w;
			if (!all) break;
		}
		if (!e) break;
		p = e + 1;
	}
}

struct ask { const char *named; struct got *g; const char *name; };

/* One provider. @returns 1 if it named tracks. */
static int ask_one(const char *name, struct got *g)
{
	const struct mm_plugin *p;
	void *priv;
	char err[256] = "";
	if (loader_find_radio(name, &p, &priv) != 0) return 0;
	struct mm_item seed = { MM_ITEM_TRACK, R.seed, R.title[0] ? R.title : NULL, R.artist[0] ? R.artist : NULL,
	                        R.album[0] ? R.album : NULL, R.duration_ms };
	int rc = p->catalog->radio(priv, &seed, MM_LIST_URIS, gather, g, err, sizeof(err));
	if (rc != 0 || !g->n) {
		LOGD(TAG, "%s: nothing after %s%s%s", name, R.seed, err[0] ? ": " : "", err);
		for (int i = 0; i < g->n; i++) free(g->uri[i]);
		g->n = 0;
		return 0;
	}
	LOGD(TAG, "%s: %d tracks after %s", name, g->n, R.seed);
	return 1;
}

static void each_other(void *ctx, const char *name)
{
	struct ask *a = ctx;
	char key[80];
	snprintf(key, sizeof(key), ",%s,", name);
	if (a->name || strstr(a->named, key)) return; /* found, or asked by name already */
	if (ask_one(name, a->g)) a->name = name;
}

static void *fetch(void *arg)
{
	(void)arg;
	struct got g = { { NULL }, 0 };
	const char *order = config_get("radio.order");
	char list[512], named[512] = ",", *save = NULL;
	const char *winner = NULL;
	snprintf(list, sizeof(list), "%s", order && *order ? order : "same, *");

	/* Names given explicitly are remembered so "*" does not ask them twice. */
	for (char *t = strtok_r(list, ", \t", &save); t && !winner; t = strtok_r(NULL, ", \t", &save)) {
		if (!strcmp(t, "*")) {
			struct ask a = { named, &g, NULL };
			loader_each_radio(each_other, &a);
			winner = a.name;
			continue;
		}
		const char *name = !strcmp(t, "same") ? R.source : t;
		if (!*name) continue;
		size_t n = strlen(named);
		snprintf(named + n, sizeof(named) - n, "%s,", name);
		if (ask_one(name, &g)) winner = name;
	}

	if (!winner) {
		LOGI(TAG, "nothing to follow %s", R.seed);
	} else {
		long want = batch(), added = 0;
		for (int i = 0; i < g.n && added < want; i++) {
			if (!strcmp(g.uri[i], R.seed) || queue_contains(g.uri[i])) continue;
			if (queue_add(g.uri[i]) < 0) break;
			added++;
		}
		LOGI(TAG, "%ld tracks from %s after %s", added, winner, R.seed);
	}
	for (int i = 0; i < g.n; i++) free(g.uri[i]);
	atomic_store(&R.done, 1);
	return NULL;
}

void radio_tick(void)
{
	if (R.th_on) {
		if (!atomic_load(&R.done)) return;
		pthread_join(R.th, NULL);
		R.th_on = 0;
	}
	if (!enabled() || player_state() != PLAYER_PLAYING) return;
	uint32_t item = player_item();
	if (!item || item == R.seeded || !queue_on_last(R.seed, sizeof(R.seed))) return;
	R.seeded = item;
	char uri[sizeof(R.seed)];
	player_current(uri, sizeof(uri), R.tags, sizeof(R.tags), &R.duration_ms, R.source, sizeof(R.source));
	if (strcmp(uri, R.seed) != 0) return; /* moved on between the two looks */
	tag(R.tags, "title", 0, R.title, sizeof(R.title));
	tag(R.tags, "artist", 1, R.artist, sizeof(R.artist));
	tag(R.tags, "album", 0, R.album, sizeof(R.album));
	atomic_store(&R.done, 0);
	R.th_on = pthread_create(&R.th, NULL, fetch, NULL) == 0;
}

static int v_radio(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	if (c->argc == 2 && (!strcmp(c->argv[1], "on") || !strcmp(c->argv[1], "off"))) {
		atomic_store(&R.off, !strcmp(c->argv[1], "off"));
		atomic_store(&R.verb, 1);
	} else if (c->argc != 1) {
		snprintf(c->reply, c->reply_len, "usage: radio [on|off]");
		return MM_ERR;
	}
	snprintf(c->reply, c->reply_len, "%s", enabled() ? "on" : "off");
	return MM_OK;
}

int radio_init(void)
{
	return ipc_register("radio", "radio", "[on|off]\twhen the queue runs out, play more like its last track",
	                    v_radio, NULL);
}

void radio_fini(void)
{
	if (R.th_on) pthread_join(R.th, NULL);
	R.th_on = 0;
	ipc_unregister_owner("radio");
}

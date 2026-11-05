/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file radio.c
 * @brief Radio: the queue does not run dry.
 *
 * Once the item playing is the queue's last, the catalog of its scheme is
 * asked for "<scheme>:radio:<that item>" on a thread of its own (it may
 * take the network a while), and the tracks it names are added, those
 * already queued left out. That happens a whole track ahead, so the
 * player's usual preload carries straight on into them. A catalog
 * with no radio answers with an error and nothing happens. Each item
 * seeds radio at most once.
 *
 * Config:
 *   radio        = on | off   (also the `radio [on|off]` verb)
 *   radio.batch  = 25         tracks added at a time */

#include "radio.h"
#include "catalog.h"
#include "config.h"
#include "ipc.h"
#include "log.h"
#include "player.h"
#include "queue.h"
#include "uri.h"
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
	char seed[2048];
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

static void *fetch(void *arg)
{
	(void)arg;
	char scheme[32], uri[2100], err[256];
	struct got g = { { NULL }, 0 };
	uri_scheme(R.seed, scheme, sizeof(scheme));
	snprintf(uri, sizeof(uri), "%s:radio:%s", scheme, R.seed);

	if (catalog_list(uri, MM_LIST_URIS, gather, &g, err, sizeof(err)) != 0) {
		LOGD(TAG, "none after %s: %s", R.seed, err);
	} else {
		long want = batch(), added = 0;
		for (int i = 0; i < g.n && added < want; i++) {
			if (!strcmp(g.uri[i], R.seed) || queue_contains(g.uri[i])) continue;
			if (queue_add(g.uri[i]) < 0) break;
			added++;
		}
		LOGI(TAG, "%ld tracks after %s", added, R.seed);
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

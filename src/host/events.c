/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file events.c
 * @brief Snapshot-diff event detection. */

#include "events.h"
#include "log.h"
#include "player.h"
#include "queue.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define MAX_SUBS 32

struct sub {
	char owner[64];
	uint32_t mask;
	mm_event_fn cb;
	void *priv;
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sub s_subs[MAX_SUBS];
static int s_nsubs;

static struct {
	int valid;
	int state;
	uint32_t item;
	int volume;
	uint32_t seeks;
	uint32_t metas;
	uint64_t queue_gen;
} s_last;

int events_subscribe(const char *owner, uint32_t mask, mm_event_fn cb, void *priv)
{
	if (!owner || !cb || !mask) return -1;
	pthread_mutex_lock(&s_lock);
	int rc = -1;
	if (s_nsubs < MAX_SUBS) {
		struct sub *e = &s_subs[s_nsubs++];
		snprintf(e->owner, sizeof(e->owner), "%s", owner);
		e->mask = mask;
		e->cb = cb;
		e->priv = priv;
		rc = 0;
	} else {
		LOGW("events", "%s: subscriber table full", owner);
	}
	pthread_mutex_unlock(&s_lock);
	return rc;
}

void events_unsubscribe_owner(const char *owner)
{
	pthread_mutex_lock(&s_lock);
	int w = 0;
	for (int i = 0; i < s_nsubs; i++)
		if (strcmp(s_subs[i].owner, owner)) s_subs[w++] = s_subs[i];
	s_nsubs = w;
	pthread_mutex_unlock(&s_lock);
}

uint32_t events_poll(void)
{
	int state = player_state();
	uint32_t item = player_item();
	int volume = player_volume();
	uint32_t seeks = player_seek_count();
	uint32_t metas = player_meta_count();
	uint64_t qgen = queue_generation();

	uint32_t ev = 0;
	if (s_last.valid) {
		if (state != s_last.state) ev |= MM_EV_STATE;
		if (item != s_last.item) ev |= MM_EV_TRACK;
		if (volume != s_last.volume) ev |= MM_EV_VOLUME;
		if (seeks != s_last.seeks) ev |= MM_EV_SEEK;
		if (metas != s_last.metas) ev |= MM_EV_META;
		if (qgen != s_last.queue_gen) ev |= MM_EV_QUEUE;
	}
	s_last.valid = 1;
	s_last.state = state;
	s_last.item = item;
	s_last.volume = volume;
	s_last.seeks = seeks;
	s_last.metas = metas;
	s_last.queue_gen = qgen;
	if (!ev) return 0;

	/* Copy so a callback may (un)subscribe without deadlocking. */
	struct sub subs[MAX_SUBS];
	pthread_mutex_lock(&s_lock);
	int n = s_nsubs;
	memcpy(subs, s_subs, (size_t)n * sizeof(subs[0]));
	pthread_mutex_unlock(&s_lock);
	for (int i = 0; i < n; i++)
		if (subs[i].mask & ev) subs[i].cb(subs[i].priv, subs[i].mask & ev);
	return ev;
}

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file null.c
 * @brief Null sink: pulls PCM on a wall clock and throws it away.
 *
 * For headless machines and tests. Counts what it pulled so a test can
 * prove audio flowed:
 *
 *   null              pulled=<frames> nonsilent=<frames> peak=<abs> running=<0|1>
 *   null reset        zero the counters
 *
 * Config:
 *   null.period_ms = 10    pull granularity
 *   null.speed     = 1     clock multiplier (tests use >1 to run fast) */

#include "mm_plugin.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct null_priv {
	const struct mm_host *h;
	pthread_t thread;
	int running;

	pthread_mutex_t lock; /* guards everything below */
	int stop;
	mm_pull_fn pull;
	void *ctx;
	unsigned long long pulled, nonsilent;
	float peak;
};

static void ts_add_ns(struct timespec *t, long ns)
{
	t->tv_nsec += ns;
	while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static void *null_thread(void *arg)
{
	struct null_priv *s = arg;
	long period_ms = 10;
	double speed = 1.0;
	const char *v;
	if ((v = s->h->config_get(s->h, "period_ms")) && atol(v) > 0) period_ms = atol(v);
	if ((v = s->h->config_get(s->h, "speed")) && atof(v) > 0) speed = atof(v);

	uint32_t frames = (uint32_t)((long)MM_PCM_RATE * period_ms / 1000);
	long sleep_ns = (long)((double)period_ms * 1e6 / speed);
	float *buf = malloc((size_t)frames * MM_PCM_CHANNELS * sizeof(float));
	if (!buf) return NULL;

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;) {
		pthread_mutex_lock(&s->lock);
		int stop = s->stop;
		mm_pull_fn pull = s->pull;
		void *ctx = s->ctx;
		pthread_mutex_unlock(&s->lock);
		if (stop) break;

		pull(ctx, buf, frames);

		unsigned long long loud = 0;
		float peak = 0.0f;
		for (uint32_t f = 0; f < frames; f++) {
			int any = 0;
			for (uint32_t c = 0; c < MM_PCM_CHANNELS; c++) {
				float a = buf[f * MM_PCM_CHANNELS + c];
				if (a < 0) a = -a;
				if (a > 1e-6f) any = 1;
				if (a > peak) peak = a;
			}
			loud += (unsigned long long)any;
		}
		pthread_mutex_lock(&s->lock);
		s->pulled += frames;
		s->nonsilent += loud;
		if (peak > s->peak) s->peak = peak;
		pthread_mutex_unlock(&s->lock);

		ts_add_ns(&next, sleep_ns);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) { }
	}
	free(buf);
	return NULL;
}

static int null_start(void *priv, mm_pull_fn pull, void *ctx)
{
	struct null_priv *s = priv;
	if (s->running) return -1;
	pthread_mutex_lock(&s->lock);
	s->stop = 0;
	s->pull = pull;
	s->ctx = ctx;
	pthread_mutex_unlock(&s->lock);
	if (pthread_create(&s->thread, NULL, null_thread, s) != 0) return -1;
	s->running = 1;
	return 0;
}

static void null_stop(void *priv)
{
	struct null_priv *s = priv;
	if (!s->running) return;
	pthread_mutex_lock(&s->lock);
	s->stop = 1;
	pthread_mutex_unlock(&s->lock);
	pthread_join(s->thread, NULL);
	s->running = 0;
}

static int null_verb(void *priv, struct mm_verb_ctx *c)
{
	struct null_priv *s = priv;
	pthread_mutex_lock(&s->lock);
	if (c->argc == 2 && !strcmp(c->argv[1], "reset")) {
		s->pulled = s->nonsilent = 0;
		s->peak = 0.0f;
	}
	snprintf(c->reply, c->reply_len, "pulled=%llu nonsilent=%llu peak=%.4f running=%d",
	         s->pulled, s->nonsilent, (double)s->peak, s->running);
	pthread_mutex_unlock(&s->lock);
	return MM_OK;
}

static int null_init(const struct mm_host *h, void **priv)
{
	struct null_priv *s = calloc(1, sizeof(*s));
	if (!s) return -1;
	s->h = h;
	pthread_mutex_init(&s->lock, NULL);
	if (h->register_verb(h, "null", "[reset]\tnull sink counters", null_verb, s) != 0) {
		pthread_mutex_destroy(&s->lock);
		free(s);
		return -1;
	}
	*priv = s;
	return 0;
}

static void null_fini(void *priv)
{
	struct null_priv *s = priv;
	null_stop(s);
	pthread_mutex_destroy(&s->lock);
	free(s);
}

static const struct mm_sink_ops s_sink = {
	.start = null_start,
	.stop = null_stop,
};

static const struct mm_plugin s_plugin = {
	.abi = MM_ABI,
	.name = "null",
	.version = "0.2.0",
	.description = "discards audio on a wall clock",
	.init = null_init,
	.fini = null_fini,
	.sink = &s_sink,
};

MM_PLUGIN_DEFINE(null)
{
	return &s_plugin;
}

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file null.c
 * @brief The null plugin.
 *
 * Exercises the plugin plumbing end to end (load, init, config slice,
 * verb, fini) with no dependencies. */

#include "mm_plugin.h"
#include <stdio.h>
#include <stdlib.h>

struct null_priv {
	const struct mm_host *h;
	unsigned long pings; /* IPC thread only */
};

static int null_verb(void *priv, struct mm_verb_ctx *c)
{
	struct null_priv *s = priv;
	const char *greeting = s->h->config_get(s->h, "greeting");
	s->pings++;
	snprintf(c->reply, c->reply_len, "null: %s (ping %lu, %d arg%s)",
	         greeting ? greeting : "hello", s->pings, c->argc - 1, c->argc == 2 ? "" : "s");
	return MM_OK;
}

static int null_init(const struct mm_host *h, void **priv)
{
	struct null_priv *s = calloc(1, sizeof(*s));
	if (!s) return -1;
	s->h = h;
	if (h->register_verb(h, "null", "[args...]\tnull plugin liveness check", null_verb, s) != 0) {
		free(s);
		return -1;
	}
	*priv = s;
	return 0;
}

static void null_fini(void *priv)
{
	free(priv);
}

static const struct mm_plugin s_plugin = {
	.abi = MM_ABI,
	.attrs = 0,
	.name = "null",
	.version = "0.1.0",
	.description = "does nothing, reliably",
	.init = null_init,
	.fini = null_fini,
};

MM_PLUGIN_DEFINE(null)
{
	return &s_plugin;
}

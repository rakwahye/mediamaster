/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file own.c
 * @brief Swapping a named track for your own copy of it. */

#include "own.h"
#include "catalog.h"
#include "config.h"
#include "loader.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "own"

static const char *prefer(void)
{
	const char *v = config_get("own.prefer");
	return v ? v : "library";
}

/* fn(name) for each name in own.prefer that is an active plugin with a
 * match op, until fn returns nonzero. @returns what fn last returned. */
static int each_owner(int (*fn)(void *ctx, const struct mm_plugin *p, void *priv), void *ctx)
{
	char list[256], *save = NULL;
	snprintf(list, sizeof(list), "%s", prefer());
	if (!strcmp(list, "off")) return 0;
	for (char *t = strtok_r(list, ", \t", &save); t; t = strtok_r(NULL, ", \t", &save)) {
		const struct mm_plugin *p;
		void *priv;
		if (loader_find_match(t, &p, &priv) != 0) continue;
		int rc = fn(ctx, p, priv);
		if (rc) return rc;
	}
	return 0;
}

static int any(void *ctx, const struct mm_plugin *p, void *priv)
{
	(void)ctx; (void)p; (void)priv;
	return 1;
}

int own_wanted(void)
{
	return each_owner(any, NULL);
}

/* "m:ss" or "h:mm:ss" → ms; 0 if not one. */
static unsigned long long parse_dur(const char *s)
{
	unsigned long long v = 0, part = 0;
	int digits = 0;
	for (; *s; s++) {
		if (*s >= '0' && *s <= '9') {
			part = part * 10 + (unsigned)(*s - '0');
			digits = 1;
		} else if (*s == ':' && digits) {
			v = (v + part) * 60;
			part = 0;
			digits = 0;
		} else {
			return 0;
		}
	}
	return digits ? (v + part) * 1000 : 0;
}

struct want { const struct mm_item *it; char uri[4096]; };

static int ask(void *ctx, const struct mm_plugin *p, void *priv)
{
	struct want *w = ctx;
	return p->catalog->match(priv, w->it, w->uri, sizeof(w->uri)) == 1;
}

int own_match(const char *uri, const char *label, char *out, size_t cap)
{
	char f[4][512];
	int n = 0;
	const char *s = label;
	if (!*label) return 0; /* nothing to match by */
	for (; n < 4; n++) {
		const char *tab = strchr(s, '\t');
		size_t len = tab ? (size_t)(tab - s) : strlen(s);
		snprintf(f[n], sizeof(f[n]), "%.*s", (int)len, s);
		if (!tab) {
			n++;
			break;
		}
		s = tab + 1;
	}
	for (int i = n; i < 4; i++) f[i][0] = '\0';
	if (!f[0][0]) return 0;

	struct mm_item it = { MM_ITEM_TRACK, uri, f[0], f[1][0] ? f[1] : NULL, f[2][0] ? f[2] : NULL, parse_dur(f[3]) };
	struct want w = { &it, "" };
	if (!each_owner(ask, &w) || !w.uri[0] || !strcmp(w.uri, uri)) return 0;
	LOGD(TAG, "%s is yours: %s", uri, w.uri);
	snprintf(out, cap, "%s", w.uri);
	return 1;
}

int own_swap(char **entry)
{
	const char *label = catalog_label(*entry);
	char mine[4096];
	if (!own_match(*entry, label, mine, sizeof(mine))) return 0;
	size_t ul = strlen(mine), ll = strlen(label);
	char *e = malloc(ul + ll + 2);
	if (!e) return 0;
	memcpy(e, mine, ul + 1);
	memcpy(e + ul + 1, label, ll + 1);
	free(*entry);
	*entry = e;
	return 1;
}

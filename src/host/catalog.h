/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file catalog.h
 * @brief Lists through catalog plugins: `browse`, and the tracks a
 * playlist, album or folder stands for. */

#ifndef MM_CATALOG_H
#define MM_CATALOG_H

#include "mm_plugin.h"

/** List `uri` with the catalog of its scheme.
 * @returns 0; MM_NOT_A_LIST; or -1 with err set (no catalog for the
 * scheme, or the plugin's own error). */
int catalog_list(const char *uri, uint32_t flags, mm_item_fn emit, void *ctx, char *err, size_t errlen);

/** The track URIs `uri` stands for, in order: `uri` itself when it is a
 * track (or no catalog knows its scheme); otherwise every track inside,
 * going into nested lists. At most `max`. *out is malloc'd, each URI
 * too; free with catalog_free. @returns the count (0 for an empty
 * list), or -1 with err set. */
long catalog_tracks(const char *uri, char ***out, long max, char *err, size_t errlen);
void catalog_free(char **v, long n);

/** Register `browse`. */
int catalog_init(void);

#endif

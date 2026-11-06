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
 * going into nested lists, each swapped for your own copy when a plugin
 * in own.prefer holds one (own.h). At most `max`. *out is malloc'd, each URI
 * too; free with catalog_free. Each URI is followed, past its NUL, by
 * the label the catalog gave it for free (catalog_label). @returns the
 * count (0 for an empty list), or -1 with err set. */
long catalog_tracks(const char *uri, char ***out, long max, char *err, size_t errlen);
void catalog_free(char **v, long n);
/** The label after a URI from catalog_tracks: "title\tartist\talbum\tduration"
 * as queue_label_make makes it, "" when nothing came with it. */
const char *catalog_label(const char *entry);
/** An item's fields as such a label. */
void catalog_item_label(const struct mm_item *it, char *out, size_t cap);

/** 1 if `text` is words to search for rather than a URI or a path: it
 * is not a path, and no plugin claims the scheme it seems to have. */
int catalog_is_query(const char *text);

/** The first track any catalog finds for `query`, into uri.
 * @returns 0, or -1 with err set. */
int catalog_first_hit(const char *query, char *uri, size_t cap, char *err, size_t errlen);

/** Register `browse` and `search`. */
int catalog_init(void);

#endif

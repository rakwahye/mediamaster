/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file own.h
 * @brief Play your own copy: a track that a list or radio names is
 * swapped for the same recording held by a plugin in own.prefer.
 *
 * Config:
 *   own.prefer = library   plugins asked, in order, by name; "off" for none */

#ifndef MM_OWN_H
#define MM_OWN_H

#include <stddef.h>

/** 1 if some plugin in own.prefer is active and can match: lists should
 * then be expanded with titles, so there is something to match by. */
int own_wanted(void);

/** Your copy of `uri`, judged by its label ("title\tartist\talbum\t
 * duration"), into out. @returns 1 if a preferred plugin holds it. */
int own_match(const char *uri, const char *label, char *out, size_t cap);

/** An entry as catalog_tracks makes them ("uri\0label\0"): if a preferred
 * plugin holds the same recording, *entry becomes its URI (the label
 * kept) and the old one is freed. @returns 1 if swapped, 0 if not. */
int own_swap(char **entry);

#endif

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file uri.h
 * @brief URI helpers shared by the player and the catalog. */

#ifndef MM_URI_H
#define MM_URI_H

#include <stddef.h>

/** "scheme:rest" → scheme; anything else (a bare path) is "file". */
void uri_scheme(const char *uri, char *out, size_t cap);

#endif

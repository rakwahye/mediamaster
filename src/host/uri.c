/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file uri.c
 * @brief URI helpers shared by the player and the catalog. */

#include "uri.h"
#include <stdio.h>
#include <string.h>

/* "scheme:rest" → scheme; a bare path is "file". */
void uri_scheme(const char *uri, char *out, size_t cap)
{
	const char *c = strchr(uri, ':');
	size_t n = c ? (size_t)(c - uri) : 0;
	int ok = n > 0 && n < cap;
	for (size_t i = 0; ok && i < n; i++) {
		char ch = uri[i];
		ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		     (i && ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.'));
	}
	if (ok) {
		memcpy(out, uri, n);
		out[n] = '\0';
	} else {
		snprintf(out, cap, "file");
	}
}

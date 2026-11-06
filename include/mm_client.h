/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file mm_client.h
 * @brief What a client of the daemon needs: where it listens, and
 * whether it is one the client was built for.
 *
 * The protocol is text over a unix socket: a command per frame, a reply
 * framed "ok <text>\n" or "err <text>\n", `help` listing the verbs. Its
 * version is the ABI's (mm_plugin.h): a client compiles MM_ABI in from
 * the SDK it was built against, asks the daemon `version`, which answers
 *
 *     mediamaster <version> abi <major>.<minor>
 *
 * and keeps talking only if MM_ABI_FITS(MM_ABI, what the daemon said).
 * mm_version_read reads that reply; mm_abi_parse the number alone.
 *
 * Strict C99, header only: a client compiles no host source. */

#ifndef MM_CLIENT_H
#define MM_CLIENT_H

#include "mm_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MM_CLIENT_NAME "mediamaster"

/** Where the daemon listens, unless told otherwise with --socket or the
 * like: $MM_SOCK, else $XDG_RUNTIME_DIR/mediamaster.sock, else
 * /tmp/mediamaster-<uid>.sock. Written into buf. @returns buf, or NULL
 * when cap is too small for the path. */
static inline const char *mm_client_socket(char *buf, size_t cap)
{
	const char *env = getenv("MM_SOCK");
	const char *rt = getenv("XDG_RUNTIME_DIR");
	int n;
	if (env && *env) n = snprintf(buf, cap, "%s", env);
	else if (rt && rt[0] == '/') n = snprintf(buf, cap, "%s/" MM_CLIENT_NAME ".sock", rt);
	else n = snprintf(buf, cap, "/tmp/" MM_CLIENT_NAME "-%u.sock", (unsigned)getuid());
	return n >= 0 && (size_t)n < cap ? buf : NULL;
}

/** Read "<major>.<minor>" from s, nothing else around it but trailing
 * whitespace. @returns 0 with *abi set (MM_ABI_MAKE), or -1. */
static inline int mm_abi_parse(const char *s, uint32_t *abi)
{
	char *end;
	unsigned long major, minor;
	if (!s || *s < '0' || *s > '9') return -1;
	major = strtoul(s, &end, 10);
	if (*end != '.' || end[1] < '0' || end[1] > '9') return -1;
	minor = strtoul(end + 1, &end, 10);
	while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
	if (*end || major > 0xffffu || minor > 0xffffu) return -1;
	*abi = MM_ABI_MAKE(major, minor);
	return 0;
}

/** The daemon's reply to `version`, without the ok/err framing. */
struct mm_version {
	char name[32];    /* "mediamaster" */
	char version[32]; /* the daemon's own, e.g. "0.1.0" */
	uint32_t abi;     /* the ABI it offers, as MM_ABI_MAKE */
};

/** Read a `version` reply. @returns 0, or -1 if `text` is not
 * "<name> <version> abi <major>.<minor>". */
static inline int mm_version_read(const char *text, struct mm_version *v)
{
	char word[8], abi[16];
	int n = 0;
	memset(v, 0, sizeof(*v));
	if (sscanf(text, "%31s %31s %7s %15s%n", v->name, v->version, word, abi, &n) != 4 || strcmp(word, "abi"))
		return -1;
	while (text[n] == ' ' || text[n] == '\n' || text[n] == '\r') n++;
	if (text[n]) return -1;
	return mm_abi_parse(abi, &v->abi);
}

#ifdef __cplusplus
}
#endif

#endif

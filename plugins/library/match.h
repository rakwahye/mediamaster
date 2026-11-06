/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file match.h
 * @brief Is this file the recording someone asked for by name?
 *
 * Pure functions, no index: the library runs them over its tracks. */

#ifndef LIB_MATCH_H
#define LIB_MATCH_H

#include <stddef.h>
#include <stdint.h>

#define MATCH_MS 3000 /* lengths this close are the same recording */

/** For comparing: lower case (ASCII), words of letters and digits (and
 * any non-ASCII byte) joined by single spaces, everything else dropped. */
void match_norm(const char *in, char *out, size_t cap);

/** A title's core: what comes before " - ", "(" or "[" (when anything
 * does), normalised. "Song - Remastered 2011" and "Song (feat. X)" are
 * both "song". */
void match_title(const char *in, char *out, size_t cap);

/** The first artist of a list ("A, B", "A & B", "A feat. B"), normalised. */
void match_artist(const char *in, char *out, size_t cap);

/** What is asked for, prepared once. */
struct match_want {
	char title[512], artist[256];
	uint64_t ms;
};
void match_prepare(struct match_want *w, const char *title, const char *artist, uint64_t ms);

/** Does a file with these tags (any may be "") and this path below the
 * music folder hold `w`? Tagged: same core title, either first artist in
 * the other's artists, lengths within MATCH_MS when both are known.
 * Untagged: its path holds the artist and the title as whole words, and
 * the lengths must agree. */
int match_track(const struct match_want *w, const char *title, const char *artist, const char *albumartist,
                const char *rel, uint64_t ms);

#endif

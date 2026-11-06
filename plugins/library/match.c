/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file match.c
 * @brief Matching a named recording to a file. */

#include "match.h"
#include <stdio.h>
#include <string.h>

void match_norm(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	int gap = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && o + 2 < cap; p++) {
		unsigned c = *p;
		int word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
		if (c >= 'A' && c <= 'Z') {
			c += 32;
			word = 1;
		}
		if (c == '\'') continue; /* "don't" is "dont" */
		if (!word) {
			gap = 1;
			continue;
		}
		if (gap && o) out[o++] = ' ';
		gap = 0;
		out[o++] = (char)c;
	}
	out[o] = '\0';
}

void match_title(const char *in, char *out, size_t cap)
{
	size_t n = strlen(in);
	const char *cut[] = { strstr(in, " - "), strchr(in, '('), strchr(in, '[') };
	for (int i = 0; i < 3; i++)
		if (cut[i] && cut[i] > in && (size_t)(cut[i] - in) < n) n = (size_t)(cut[i] - in);
	char core[1024];
	snprintf(core, sizeof(core), "%.*s", (int)n, in);
	match_norm(core, out, cap);
	if (!out[0]) match_norm(in, out, cap); /* the whole was in brackets */
}

void match_artist(const char *in, char *out, size_t cap)
{
	char low[1024];
	match_norm(in, low, sizeof(low));
	/* Cut at the first separator word or mark of a second artist. */
	size_t n = strlen(in);
	const char *seps[] = { ", ", " & ", " feat", " ft.", " x ", " and ", " vs", ";", "/" };
	for (size_t i = 0; i < sizeof(seps) / sizeof(seps[0]); i++) {
		const char *s = strstr(in, seps[i]);
		if (s && s > in && (size_t)(s - in) < n) n = (size_t)(s - in);
	}
	char first[1024];
	snprintf(first, sizeof(first), "%.*s", (int)n, in);
	match_norm(first, out, cap);
	if (!out[0]) snprintf(out, cap, "%s", low);
}

void match_prepare(struct match_want *w, const char *title, const char *artist, uint64_t ms)
{
	match_title(title ? title : "", w->title, sizeof(w->title));
	match_artist(artist ? artist : "", w->artist, sizeof(w->artist));
	w->ms = ms;
}

/* `needle` in `hay` as whole words (both normalised). */
static int has_words(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	if (!n) return 0;
	for (const char *p = hay; (p = strstr(p, needle)); p++) {
		int start = p == hay || p[-1] == ' ', end = p[n] == '\0' || p[n] == ' ';
		if (start && end) return 1;
	}
	return 0;
}

static int lengths_agree(uint64_t a, uint64_t b)
{
	uint64_t d = a > b ? a - b : b - a;
	return d <= MATCH_MS;
}

int match_track(const struct match_want *w, const char *title, const char *artist, const char *albumartist,
                const char *rel, uint64_t ms)
{
	if (!w->title[0] || !w->artist[0]) return 0;
	if (title && title[0]) {
		char t[512], a[1024], aa[1024];
		match_title(title, t, sizeof(t));
		if (strcmp(t, w->title) != 0) return 0;
		match_norm(artist ? artist : "", a, sizeof(a));
		match_norm(albumartist ? albumartist : "", aa, sizeof(aa));
		char first[256];
		match_artist(artist && artist[0] ? artist : albumartist ? albumartist : "", first, sizeof(first));
		int same_artist = has_words(a, w->artist) || has_words(aa, w->artist) ||
		                  (first[0] && has_words(w->artist, first));
		if (!same_artist) return 0;
		return !w->ms || !ms || lengths_agree(w->ms, ms);
	}
	/* Untagged: the path must say it, and the lengths must agree. */
	if (!w->ms || !ms || !lengths_agree(w->ms, ms)) return 0;
	char path[4096];
	match_norm(rel, path, sizeof(path));
	return has_words(path, w->artist) && has_words(path, w->title);
}

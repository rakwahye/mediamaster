/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Matching a recording named by title, artist and length to a file. */
#include "match.h"
#include "check.h"
#include <string.h>

static int same(void (*fn)(const char *, char *, size_t), const char *in, const char *want)
{
	char out[512];
	fn(in, out, sizeof(out));
	return !strcmp(out, want);
}

static int yes(const char *wt, const char *wa, uint64_t wms, const char *t, const char *a, const char *aa,
               const char *rel, uint64_t ms)
{
	struct match_want w;
	match_prepare(&w, wt, wa, wms);
	return match_track(&w, t, a, aa, rel, ms);
}

int main(void)
{
	CHECK(same(match_norm, "  Don't Stop -- Me_Now! ", "dont stop me now"), "normalised");
	CHECK(same(match_norm, "Björk", "bj\xc3\xb6rk"), "non-ASCII kept as it is");
	CHECK(same(match_title, "Time - 2011 Remaster", "time"), "remaster suffix");
	CHECK(same(match_title, "Roller Mobster (feat. X)", "roller mobster"), "featuring in brackets");
	CHECK(same(match_title, "(Untitled)", "untitled"), "all in brackets: the whole");
	CHECK(same(match_artist, "TINFED, Chino Moreno", "tinfed"), "first of a list");
	CHECK(same(match_artist, "Simon & Garfunkel", "simon"), "an ampersand splits too");
	CHECK(same(match_artist, "AC/DC", "ac"), "a slash splits (acceptable: AC/DC matches on title and length)");

	/* Tagged files. */
	CHECK(yes("Time - 2011 Remaster", "Pink Floyd", 413000, "Time", "Pink Floyd", "", "PF/DSOTM/04 Time.flac", 412000),
	      "same recording, a remaster title, a second apart");
	CHECK(!yes("Time", "Pink Floyd", 413000, "Time", "Pink Floyd", "", "x", 200000), "another length: another recording");
	CHECK(yes("Time", "Pink Floyd", 413000, "Time", "Pink Floyd", "", "x", 0), "length not known yet: by tags");
	CHECK(!yes("Time", "Pink Floyd", 0, "Time", "Hans Zimmer", "", "x", 0), "another artist");
	CHECK(yes("DANGERGIRL", "TINFED, Chino Moreno", 230000, "Dangergirl", "TINFED", "", "x", 229500),
	      "the first of several artists");
	CHECK(yes("Song", "Artist", 0, "Song", "", "Artist", "x", 0), "the album artist counts");
	CHECK(!yes("Song", "Art", 0, "Song", "Artist", "", "x", 0), "whole words: Art is not Artist");

	/* Untagged files: the path, and the length must agree. */
	const char *rel = "rips/converted/Carpenter_Brut-Leather_Terror-02-Straight_Outta_Hell.mp3";
	CHECK(yes("Straight Outta Hell", "Carpenter Brut", 214000, "", "", "", rel, 215500), "a rip found by its path");
	CHECK(!yes("Straight Outta Hell", "Carpenter Brut", 0, "", "", "", rel, 215500), "but not without a length");
	CHECK(!yes("Straight Outta Hell", "Carpenter Brut", 300000, "", "", "", rel, 215500), "nor with another");
	CHECK(!yes("Hell", "Carpenter Brut", 215000, "", "", "", "x/Carpenter_Brut-Hellfire.mp3", 215000),
	      "whole words in the path too");
	CHECK(!yes("", "Carpenter Brut", 215000, "", "", "", rel, 215500), "nothing asked for");
	CHECK_PASS("test-libmatch");
}

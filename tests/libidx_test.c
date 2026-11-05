/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The library index on a real folder tree: what it holds, that a link
 * back up the tree is followed once, that a restart answers from the
 * saved index at once, that a rescan reads only changed folders and
 * probes only new or changed files (a retag in place included), and that
 * stopped and long scans lose nothing. Tag checks need ffmpeg; without
 * it the test passes (skips). */
#include "index.h"
#include "check.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void h_log(const struct mm_host *h, int level, const char *fmt, ...)
{
	(void)h;
	if (level > MM_LOG_WARN && !getenv("VERBOSE")) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
static struct mm_host H = { .abi = MM_ABI, .size = sizeof(struct mm_host), .log = h_log };

static char D[128], ROOT[160], SAVED[160];

static int sh(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

static void mk(const char *rel) { sh("mkdir -p '%s/%s'", ROOT, rel); }
static void touch(const char *rel) { sh(": > '%s/%s'", ROOT, rel); }

/* A one-second FLAC tagged with title and artist. */
static int tone(const char *rel, const char *title, const char *artist)
{
	return sh("ffmpeg -v error -f lavfi -i sine=d=1 -metadata title='%s' -metadata artist='%s' "
	          "-metadata track=2/9 -y '%s/%s'", title, artist, ROOT, rel);
}

/* Every track as "rel=title," in index order; "?" for no title. */
static char R[8192];
static int collect(void *ctx, const struct libidx_track *t)
{
	char title[256];
	(void)ctx;
	libidx_tag(t->tags, "title", title, sizeof(title));
	size_t n = strlen(R);
	snprintf(R + n, sizeof(R) - n, "%s=%s,", t->rel, title[0] ? title : "?");
	return 0;
}
static const char *all(struct libidx *x)
{
	R[0] = '\0';
	libidx_each(x, collect, NULL);
	return R;
}

static int settle(struct libidx *x, long scans, struct libidx_stats *st)
{
	for (int i = 0; i < 20000; i++) {
		libidx_stats(x, st);
		if (st->scans >= scans && !st->scanning) return 0;
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	return -1;
}

static void tick(void)
{
	struct timespec ts = { 0, 20 * 1000000 };
	nanosleep(&ts, NULL);
}

int main(void)
{
	struct libidx_stats st;
	struct libidx *x;
	if (system("ffmpeg -version >/dev/null 2>&1") != 0 || system("ffprobe -version >/dev/null 2>&1") != 0) {
		printf("test-libidx: SKIP (ffmpeg/ffprobe not installed)\n");
		return 0;
	}
	snprintf(D, sizeof(D), "/tmp/mm-libidx-test.%d", (int)getpid());
	snprintf(ROOT, sizeof(ROOT), "%s/music", D);
	snprintf(SAVED, sizeof(SAVED), "%s/library.idx", D);
	mk("A");
	mk("B/C");
	mk(".hidden");
	CHECK(tone("A/10 Ten.flac", "Ten", "Osc") == 0 && tone("A/2 Two.flac", "Two", "Osc") == 0, "fixtures");
	touch("A/notes.txt");
	touch("B/C/x.opus"); /* not really audio: probed, no tags */
	touch(".hidden/h.flac");
	char link[512];
	snprintf(link, sizeof(link), "%s/A/up", ROOT);
	CHECK(symlink("..", link) == 0, "a link back up the tree");

	/* First run: everything read and probed. */
	x = libidx_open(&H, ROOT, SAVED, "ffprobe");
	CHECK(x && settle(x, 1, &st) == 0, "first scan");
	CHECK(st.files == 3 && st.dirs == 4 && st.dirs_read == 4 && st.probes == 3 && st.probed == 3,
	      "3 audio files in 4 folders, all probed; the loop followed once");
	CHECK(!strcmp(all(x), "A/2 Two.flac=Two,A/10 Ten.flac=Ten,B/C/x.opus=?,"),
	      "tags; natural order; hidden folders and other files left out");
	libidx_close(x);
	CHECK(access(SAVED, R_OK) == 0, "saved");

	/* Restart: answers from what was saved, then a scan that reads and
	 * probes nothing. */
	x = libidx_open(&H, ROOT, SAVED, "ffprobe");
	CHECK(!strcmp(all(x), "A/2 Two.flac=Two,A/10 Ten.flac=Ten,B/C/x.opus=?,"), "at once, tags included");
	CHECK(settle(x, 1, &st) == 0 && st.dirs_read == 0 && st.probes == 0, "nothing changed: nothing read or probed");

	/* A retag in place: the folder is unchanged, the file is not. */
	tick();
	CHECK(tone("A/2 Two.flac", "Deux", "Osc") == 0, "retag");
	libidx_rescan(x);
	CHECK(settle(x, 2, &st) == 0 && st.probes == 1, "only that file probed");
	CHECK(strstr(all(x), "A/2 Two.flac=Deux,") != NULL, "the new title");

	/* A new file and a new folder. */
	tick();
	CHECK(tone("B/new.flac", "New", "Other") == 0, "a new file");
	mk("A/Disc 2");
	CHECK(tone("A/Disc 2/1 One.flac", "One", "Osc") == 0, "a new folder");
	libidx_rescan(x);
	CHECK(settle(x, 3, &st) == 0 && st.dirs_read == 3 && st.probes == 2 && st.files == 5,
	      "A, B and the new folder read; the two new files probed");
	libidx_close(x);

	/* Stopping a warm start, again and again, loses nothing. */
	for (int i = 0; i < 20; i++) libidx_close(libidx_open(&H, ROOT, SAVED, "ffprobe"));
	x = libidx_open(&H, ROOT, SAVED, "ffprobe");
	libidx_stats(x, &st);
	CHECK(st.files == 5 && st.probed == 5 && strstr(all(x), "Disc 2/1 One.flac=One"), "all still there");
	libidx_close(x);

	/* A long first scan is searchable and saved as it goes, and a stop
	 * in the middle of probing keeps the tags read so far. */
	unlink(SAVED);
	mk("Many");
	for (int i = 0; i < 60; i++) {
		char rel[64];
		snprintf(rel, sizeof(rel), "Many/t%03d.mp3", i);
		touch(rel);
	}
	x = libidx_open(&H, ROOT, SAVED, "ffprobe");
	libidx_checkpoint_every(x, 0);
	int partial = 0;
	for (int i = 0; i < 20000 && !partial; i++) {
		libidx_stats(x, &st);
		if (st.scans) break;
		if (st.files == 65 && st.probed > 0 && st.probed < 65) partial = 1; /* folders shown, tags coming */
		struct timespec ts = { 0, 200000 };
		nanosleep(&ts, NULL);
	}
	libidx_close(x); /* in the middle of probing, most likely */
	x = libidx_open(&H, ROOT, SAVED, "ffprobe");
	libidx_stats(x, &st);
	long kept = st.probed;
	CHECK(st.files == 65, "every file kept");
	CHECK(settle(x, 1, &st) == 0 && st.probed == 65 && st.probes == 65 - kept, "the rest probed, no file twice");
	libidx_close(x);
	if (!partial) fprintf(stderr, "  (the scan finished before a partial index was seen)\n");

	/* No ffprobe: files are indexed, tags wait. */
	unlink(SAVED);
	x = libidx_open(&H, ROOT, SAVED, "/nonexistent/ffprobe");
	CHECK(settle(x, 0, &st) == 0, "settles");
	for (int i = 0; i < 2000 && !st.files; i++) {
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
		libidx_stats(x, &st);
	}
	CHECK(st.files == 65 && st.probed == 0, "indexed without tags");
	libidx_close(x);

	CHECK(sh("rm -rf '%s'", D) == 0, "clean up");
	CHECK_PASS("test-libidx");
}

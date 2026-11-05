/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The music index on a real folder tree: what it finds, that a link back
 * up the tree is followed once, that a restart answers from the saved
 * index at once, and that a rescan reads only the folders that changed. */
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

static void touch(const char *rel)
{
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
	FILE *f = fopen(p, "w");
	if (f) fclose(f);
}

static void mk(const char *rel)
{
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
	mkdir(p, 0700);
}

/* Results of a search, as relative paths joined by '|'. */
static char R[4096];
static int collect(void *ctx, const struct mm_item *it)
{
	(void)ctx;
	size_t n = strlen(R);
	snprintf(R + n, sizeof(R) - n, "%s|", it->uri + strlen(ROOT) + 1);
	return 0;
}
static const char *find(struct ffidx *x, const char *words)
{
	R[0] = '\0';
	ffidx_search(x, words, collect, NULL);
	return R;
}

/* Wait until `scans` scans have completed. */
static int settle(struct ffidx *x, long scans, struct ffidx_stats *st)
{
	for (int i = 0; i < 4000; i++) {
		ffidx_stats(x, st);
		if (st->scans >= scans && !st->scanning) return 0;
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	return -1;
}

/* Folder times change by whole entries; make sure a second has a chance
 * to show on file systems with coarse times. */
static void tick(void)
{
	struct timespec ts = { 0, 20 * 1000000 };
	nanosleep(&ts, NULL);
}

int main(void)
{
	struct ffidx_stats st;
	snprintf(D, sizeof(D), "/tmp/mm-ffidx-test.%d", (int)getpid());
	snprintf(ROOT, sizeof(ROOT), "%s/music", D);
	snprintf(SAVED, sizeof(SAVED), "%s/music.idx", D);
	mkdir(D, 0700);
	mkdir(ROOT, 0700);
	mk("A"); mk("B"); mk("B/C"); mk(".hidden");
	touch("A/10 Ten.FLAC"); touch("A/2 Two.mp3"); touch("A/notes.txt");
	touch("B/C/x.opus"); touch(".hidden/h.flac");
	char link[512];
	snprintf(link, sizeof(link), "%s/A/up", ROOT);
	CHECK(symlink("..", link) == 0, "a link back up the tree");

	/* First run: nothing saved, everything read. */
	struct ffidx *x = ffidx_open(&H, ROOT, SAVED);
	CHECK(x && settle(x, 1, &st) == 0, "first scan");
	CHECK(st.files == 3 && st.dirs == 4 && st.dirs_read == 4, "3 audio files in 4 folders; the loop followed once");
	CHECK(!strcmp(find(x, "flac"), "A/10 Ten.FLAC|"), "any case; hidden folders and other files left out");
	CHECK(!strcmp(find(x, "a"), "A/2 Two.mp3|A/10 Ten.FLAC|"), "natural order within a folder");
	CHECK(!strcmp(find(x, "OPUS b"), "B/C/x.opus|"), "every word, anywhere in the path");
	CHECK(!strcmp(find(x, "zzz"), "") && !strcmp(find(x, "  "), ""), "nothing, and no words");
	ffidx_close(x);
	CHECK(access(SAVED, R_OK) == 0, "saved");

	/* Restart: answers from the saved index before any scan, then a scan
	 * that reads nothing because nothing changed. */
	x = ffidx_open(&H, ROOT, SAVED);
	CHECK(!strcmp(find(x, "x.opus"), "B/C/x.opus|"), "answers at once from what was saved");
	CHECK(settle(x, 1, &st) == 0 && st.dirs_read == 0 && st.files == 3, "unchanged: no folder read");

	/* A new file in B: only B is read again. */
	tick();
	touch("B/new.flac");
	ffidx_rescan(x);
	CHECK(settle(x, 2, &st) == 0 && st.dirs_read == 1 && st.files == 4, "one changed folder read");
	CHECK(!strcmp(find(x, "new"), "B/new.flac|"), "the new file found");

	/* A file removed and a folder added in A: A and the new folder read. */
	tick();
	char p[512];
	snprintf(p, sizeof(p), "%s/A/2 Two.mp3", ROOT);
	unlink(p);
	mk("A/Disc 2");
	touch("A/Disc 2/1 One.wav");
	ffidx_rescan(x);
	CHECK(settle(x, 3, &st) == 0 && st.dirs_read == 2 && st.files == 4, "the changed and the new folder");
	CHECK(!strcmp(find(x, "two"), "") && !strcmp(find(x, "disc one"), "A/Disc 2/1 One.wav|"), "gone, and found");
	ffidx_close(x);

	/* Another root with the same saved file starts afresh. */
	char other[200];
	snprintf(other, sizeof(other), "%s/B", ROOT);
	x = ffidx_open(&H, other, SAVED);
	CHECK(!strcmp(find(x, "flac"), ""), "another folder's index is not used");
	CHECK(settle(x, 1, &st) == 0 && st.dirs_read == 2 && st.files == 2, "read afresh");
	ffidx_close(x);

	/* Close during a scan returns. */
	x = ffidx_open(&H, ROOT, SAVED);
	ffidx_close(x);

	/* A warm start stopped mid-scan keeps all it knew. */
	x = ffidx_open(&H, ROOT, SAVED);
	CHECK(settle(x, 1, &st) == 0, "full index");
	long dirs = st.dirs, files = st.files;
	ffidx_close(x);
	for (int i = 0; i < 20; i++) {
		x = ffidx_open(&H, ROOT, SAVED);
		ffidx_close(x); /* at once, mostly before the scan gets far */
	}
	x = ffidx_open(&H, ROOT, SAVED);
	ffidx_stats(x, &st);
	CHECK(st.dirs == dirs && st.files == files, "nothing lost to stopped scans");
	ffidx_close(x);

	/* A long first scan is searchable and saved as it goes. */
	unlink(SAVED);
	for (int i = 0; i < 300; i++) {
		char rel[64];
		snprintf(rel, sizeof(rel), "Many/%03d", i);
		if (i == 0) mk("Many");
		mk(rel);
		snprintf(rel, sizeof(rel), "Many/%03d/t%03d.flac", i, i);
		touch(rel);
	}
	x = ffidx_open(&H, ROOT, SAVED);
	ffidx_checkpoint_every(x, 0);
	int seen_partial = 0;
	for (int i = 0; i < 4000 && !seen_partial; i++) {
		ffidx_stats(x, &st);
		if (!st.scans && st.files > 0) seen_partial = 1; /* a checkpoint before the end */
		else if (st.scans) break;
		struct timespec ts = { 0, 100000 };
		nanosleep(&ts, NULL);
	}
	if (seen_partial) CHECK(access(SAVED, R_OK) == 0, "a checkpoint is saved");
	CHECK(settle(x, 1, &st) == 0 && st.files == files + 300, "then the full index");
	CHECK(!strcmp(find(x, "t299"), "Many/299/t299.flac|"), "all of it searchable");
	ffidx_close(x);
	if (!seen_partial) fprintf(stderr, "  (the scan finished before a checkpoint was seen)\n");

	char cmd[256];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", D);
	CHECK(system(cmd) == 0, "clean up");
	CHECK_PASS("test-ffidx");
}

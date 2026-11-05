/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The ffmpeg source against real ffmpeg on generated fixtures, and against
 * fake binaries for the hang, interrupt and kill paths. Skips (passes) if
 * ffmpeg is not installed. */
#include "config.h"
#include "loader.h"
#include "check.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const struct mm_plugin *mm_static_entry_ffmpeg(void);

#define CH MM_PCM_CHANNELS
static const struct mm_source_ops *S;
static void *P;
static char D[64]; /* fixture dir */

static long ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


static void set(const char *kv)
{
	config_override(kv);
	config_reload();
}

struct drained { long frames; float peak; long crossings; int lr_equal; long err; };

static struct drained drain(void *st)
{
	static float buf[CH * 4096];
	struct drained d = { 0, 0, 0, 1, 0 };
	float prev = 0;
	for (;;) {
		long n = S->read(st, buf, 4096);
		if (n < 0) { d.err = n; break; }
		if (n == 0) break;
		for (long i = 0; i < n; i++) {
			float l = buf[i * CH], r = buf[i * CH + 1];
			if (fabsf(l) > d.peak) d.peak = fabsf(l);
			if (fabsf(l - r) > 1e-6f) d.lr_equal = 0;
			if (prev < 0 && l >= 0) d.crossings++;
			prev = l;
		}
		d.frames += n;
	}
	return d;
}

static int decode(void)
{
	char uri[256];
	void *st;
	struct mm_stream_info in;

	/* 44.1 kHz stereo FLAC: exercises resampling to 48 kHz. */
	snprintf(uri, sizeof(uri), "%s/tone.flac", D);
	CHECK(S->open(P, uri, &st, &in) == 0, "a local FLAC opens from a bare path");
	CHECK(in.duration_ms >= 995 && in.duration_ms <= 1005, "ffprobe reports about one second");
	CHECK(in.flags == MM_STREAM_SEEKABLE, "a file with a duration is seekable, not live");
	struct drained d = drain(st);
	CHECK(d.err == 0, "decoding finishes without error");
	CHECK(labs(d.frames - 48000) <= 64, "one second at 44.1 kHz comes out as ~48000 frames");
	CHECK(labs(d.crossings - 1000) <= 2, "the 1 kHz tone survives resampling at 1 kHz");
	CHECK(d.peak > 0.058f && d.peak < 0.067f, "level is preserved (lavfi sine is 1/8, minus 6 dB)");
	CHECK(S->read(st, (float[CH]){0}, 1) == 0, "reading after the end keeps returning 0");

	CHECK(S->seek(st, 500) == 0, "seek respawns at an offset");
	d = drain(st);
	CHECK(labs(d.frames - 24000) <= 512, "after seeking to 0.5 s about half remains");
	S->close(st);

	/* Mono MP3 under a hostile name: leading '-', spaces, a colon. */
	snprintf(uri, sizeof(uri), "file://%s/-odd name:1.mp3", D);
	CHECK(S->open(P, uri, &st, &in) == 0, "a file:// URI with a leading '-' and a ':' in the name opens");
	d = drain(st);
	CHECK(d.frames > 40000 && d.lr_equal && d.peak > 0.05f, "mono is upmixed to two equal channels (ffmpeg pans it -3 dB)");
	S->close(st);
	return 0;
}

static int refusals(void)
{
	char uri[256];
	void *st = NULL;
	struct mm_stream_info in;

	snprintf(uri, sizeof(uri), "%s/missing.flac", D);
	CHECK(S->open(P, uri, &st, &in) != 0, "a missing file is refused at open");
	CHECK(S->open(P, "relative/path.mp3", &st, &in) != 0, "a relative path is refused");
	CHECK(S->open(P, D, &st, &in) != 0, "a directory is refused");

	/* Not audio: ffprobe finds no duration, ffmpeg fails, read ends. */
	snprintf(uri, sizeof(uri), "%s/notes.txt", D);
	CHECK(S->open(P, uri, &st, &in) == 0, "a non-audio file opens (ffmpeg decides, not us)");
	CHECK(in.flags == MM_STREAM_LIVE && in.duration_ms == 0, "without a duration the stream is treated as live");
	long t0 = ms_now();
	struct drained d = drain(st);
	CHECK(d.frames == 0 && ms_now() - t0 < 3000, "ffmpeg's failure ends the stream promptly with no audio");
	S->close(st);

	set("ffmpeg.bin=/nonexistent/ffmpeg");
	snprintf(uri, sizeof(uri), "%s/tone.flac", D);
	CHECK(S->open(P, uri, &st, &in) != 0, "a missing ffmpeg binary fails open cleanly");
	set("ffmpeg.bin=ffmpeg");
	return 0;
}

static void *poke(void *st)
{
	struct timespec ts = { 0, 150 * 1000 * 1000 };
	nanosleep(&ts, NULL);
	S->interrupt(st);
	return NULL;
}

static int hangs(void)
{
	char uri[256], kv[256];
	void *st;
	struct mm_stream_info in;
	snprintf(uri, sizeof(uri), "%s/tone.flac", D);

	/* ffprobe that never answers: bounded by probe_ms. */
	snprintf(kv, sizeof(kv), "ffmpeg.ffprobe=%s/hang.sh", D);
	set(kv);
	set("ffmpeg.probe_ms=300");
	long t0 = ms_now();
	CHECK(S->open(P, uri, &st, &in) == 0, "open survives a hung ffprobe");
	CHECK(ms_now() - t0 < 1500 && in.flags == MM_STREAM_LIVE, "a hung probe times out and the stream is treated as live");
	S->close(st);
	set("ffmpeg.ffprobe=ffprobe");

	/* Decoder that never writes: interrupt unblocks read, close reaps it. */
	snprintf(kv, sizeof(kv), "ffmpeg.bin=%s/hang.sh", D);
	set(kv);
	CHECK(S->open(P, uri, &st, &in) == 0, "open with a silent decoder");
	pthread_t t;
	pthread_create(&t, NULL, poke, st);
	float buf[CH * 64];
	t0 = ms_now();
	long n = S->read(st, buf, 64);
	pthread_join(t, NULL);
	CHECK(n == 0 && ms_now() - t0 < 1000, "interrupt makes a blocked read return 0 promptly");
	t0 = ms_now();
	S->close(st);
	CHECK(ms_now() - t0 < 500, "close stops a cooperative child with SIGTERM");

	/* Decoder that ignores SIGTERM: SIGKILL after the grace period. */
	snprintf(kv, sizeof(kv), "ffmpeg.bin=%s/stubborn.sh", D);
	set(kv);
	CHECK(S->open(P, uri, &st, &in) == 0, "open with a decoder that ignores SIGTERM");
	struct timespec settle = { 0, 300 * 1000 * 1000 }; /* let the script install its trap */
	nanosleep(&settle, NULL);
	t0 = ms_now();
	S->close(st);
	long took = ms_now() - t0;
	CHECK(took >= 900 && took < 2500, "close escalates to SIGKILL after the grace period");
	set("ffmpeg.bin=ffmpeg");
	return 0;
}

int main(void)
{
	if (system("ffmpeg -version >/dev/null 2>&1") != 0 || system("ffprobe -version >/dev/null 2>&1") != 0) {
		printf("test-ffmpeg: SKIP (ffmpeg/ffprobe not installed)\n");
		return 0;
	}
	snprintf(D, sizeof(D), "/tmp/mm-ffmpeg-test.%d", (int)getpid());
	mkdir(D, 0700);
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "ffmpeg -v error -f lavfi -i sine=f=1000:r=44100:d=1 "
	         "-af 'volume=-6dB,pan=stereo|c0=c0|c1=c0' '%s/tone.flac' -y", D);
	CHECK(system(cmd) == 0, "fixture: stereo flac");
	snprintf(cmd, sizeof(cmd), "ffmpeg -v error -f lavfi -i sine=f=440:d=1 -ac 1 -q:a 4 '%s/-odd name:1.mp3' -y", D);
	CHECK(system(cmd) == 0, "fixture: mono mp3");
	snprintf(cmd, sizeof(cmd), "echo 'not audio' > '%s/notes.txt'", D);
	CHECK(system(cmd) == 0, "fixture: text");
	snprintf(cmd, sizeof(cmd), "printf '#!/bin/sh\\nexec sleep 30\\n' > '%s/hang.sh' && chmod +x '%s/hang.sh'", D, D);
	CHECK(system(cmd) == 0, "fixture: hang");
	snprintf(cmd, sizeof(cmd), "printf '#!/bin/sh\\ntrap \"\" TERM\\nwhile :; do sleep 0.05; done\\n' > '%s/stubborn.sh' && chmod +x '%s/stubborn.sh'", D, D);
	CHECK(system(cmd) == 0, "fixture: stubborn");

	config_override("ffmpeg.index=off"); /* do not index the real music folder */
	config_load(NULL);
	CHECK(loader_add(mm_static_entry_ffmpeg(), NULL, "builtin") == 0, "plugin accepted");
	loader_init_all();
	const struct mm_plugin *p;
	CHECK(loader_find_source("file", &p, &P) == 0 && loader_find_source("https", &p, &P) == 0, "it claims file, http and https");
	S = p->source;

	RUN(decode);
	RUN(refusals);
	RUN(hangs);

	loader_fini_all();
	config_free();
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", D);
	if (system(cmd) != 0) { /* best effort */ }
	CHECK_PASS("test-ffmpeg");
}

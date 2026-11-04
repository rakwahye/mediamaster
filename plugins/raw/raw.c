/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file raw.c
 * @brief Dependency-free sources for tests and diagnostics.
 *
 *   sine:<hz>[:<seconds>]   tone at -14 dBFS; endless without seconds
 *   raw:<path>              interleaved float32 stereo 48 kHz, no header
 *
 * Both are seekable (a finite sine, a file). */

#include "mm_plugin.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SINE_AMP 0.2f
#define FRAME_BYTES (MM_PCM_CHANNELS * sizeof(float))

struct raw_stream {
	FILE *f;            /* raw: */
	double hz;          /* sine: */
	uint64_t pos;       /* frames emitted */
	uint64_t total;     /* frames; 0 = endless */
};

static int open_sine(const char *spec, struct raw_stream *s, struct mm_stream_info *info)
{
	char *end;
	double hz = strtod(spec, &end);
	if (end == spec || hz <= 0 || hz >= MM_PCM_RATE / 2.0) return -1;
	double secs = 0;
	if (*end == ':') {
		const char *p = end + 1;
		secs = strtod(p, &end);
		if (end == p || secs <= 0) return -1;
	}
	if (*end) return -1;
	s->hz = hz;
	s->total = (uint64_t)(secs * MM_PCM_RATE + 0.5);
	info->duration_ms = (uint64_t)(secs * 1000.0 + 0.5);
	info->flags = s->total ? MM_STREAM_SEEKABLE : MM_STREAM_LIVE;
	return 0;
}

static int open_raw(const char *path, struct raw_stream *s, struct mm_stream_info *info)
{
	struct stat st;
	s->f = fopen(path, "rb");
	if (!s->f) return -1;
	if (fstat(fileno(s->f), &st) != 0 || !S_ISREG(st.st_mode)) {
		fclose(s->f);
		s->f = NULL;
		return -1;
	}
	s->total = (uint64_t)st.st_size / FRAME_BYTES;
	info->duration_ms = s->total * 1000u / MM_PCM_RATE;
	info->flags = MM_STREAM_SEEKABLE;
	return 0;
}

static int raw_open(void *priv, const char *uri, void **stream, struct mm_stream_info *info)
{
	(void)priv;
	struct raw_stream *s = calloc(1, sizeof(*s));
	if (!s) return -1;
	int rc = -1;
	if (!strncmp(uri, "sine:", 5)) rc = open_sine(uri + 5, s, info);
	else if (!strncmp(uri, "raw:", 4)) rc = open_raw(uri + 4, s, info);
	if (rc != 0) {
		free(s);
		return -1;
	}
	*stream = s;
	return 0;
}

static long raw_read(void *stream, float *dst, uint32_t frames)
{
	struct raw_stream *s = stream;
	if (s->total && s->pos >= s->total) return 0;
	if (s->total && s->total - s->pos < frames) frames = (uint32_t)(s->total - s->pos);

	if (s->f) {
		size_t n = fread(dst, FRAME_BYTES, frames, s->f);
		if (n == 0 && ferror(s->f)) return -EIO;
		s->pos += n;
		return (long)n;
	}
	const double w = 2.0 * 3.14159265358979323846 * s->hz / MM_PCM_RATE;
	for (uint32_t i = 0; i < frames; i++) {
		/* Phase from the absolute position: exact after any seek. */
		float v = SINE_AMP * (float)sin(w * (double)(s->pos + i));
		for (uint32_t c = 0; c < MM_PCM_CHANNELS; c++) dst[i * MM_PCM_CHANNELS + c] = v;
	}
	s->pos += frames;
	return (long)frames;
}

static int raw_seek(void *stream, uint64_t ms)
{
	struct raw_stream *s = stream;
	uint64_t to = ms * MM_PCM_RATE / 1000u;
	if (s->total && to > s->total) to = s->total;
	if (s->f && fseek(s->f, (long)(to * FRAME_BYTES), SEEK_SET) != 0) return -1;
	s->pos = to;
	return 0;
}

static void raw_close(void *stream)
{
	struct raw_stream *s = stream;
	if (s->f) fclose(s->f);
	free(s);
}

static int raw_init(const struct mm_host *h, void **priv)
{
	(void)h;
	*priv = NULL;
	return 0;
}

static const struct mm_source_ops s_source = {
	.open = raw_open,
	.read = raw_read,
	.seek = raw_seek,
	.close = raw_close,
};

static const struct mm_plugin s_plugin = {
	.abi = MM_ABI,
	.name = "raw",
	.version = "0.1.0",
	.description = "sine generator and raw f32 files",
	.schemes = "sine,raw",
	.init = raw_init,
	.source = &s_source,
};

MM_PLUGIN_DEFINE(raw)
{
	return &s_plugin;
}

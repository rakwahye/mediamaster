/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file levels.c
 * @brief Peaks and a log-spaced spectrum from the tap. */

#include "levels.h"
#include "player.h"
#include "mm_plugin.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CH MM_PCM_CHANNELS
#define N LEVELS_FFT
#define FLOOR_DB 72.0

static float s_hist[N]; /* newest mono audio, oldest first */
static int s_on;
static int s_quiet = 1; /* the last line said silence, or none was sent */

/* ------------------------------------------------------------------ spectrum */

static float s_win[N];
static float s_twr[N / 2], s_twi[N / 2]; /* e^(-2 pi i k / N) */
static unsigned s_lo[LEVELS_BANDS], s_hi[LEVELS_BANDS]; /* bins, inclusive */

static void setup(void)
{
	static int done;
	if (done) return;
	for (unsigned i = 0; i < N; i++) s_win[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / (N - 1)));
	for (unsigned k = 0; k < N / 2; k++) {
		s_twr[k] = (float)cos(-2.0 * M_PI * k / N);
		s_twi[k] = (float)sin(-2.0 * M_PI * k / N);
	}
	const double df = (double)MM_PCM_RATE / N, lo = 40.0, hi = 16000.0;
	for (unsigned k = 0; k < LEVELS_BANDS; k++) {
		double a = lo * pow(hi / lo, (double)k / LEVELS_BANDS);
		double b = lo * pow(hi / lo, (double)(k + 1) / LEVELS_BANDS);
		unsigned first = (unsigned)ceil(a / df), last = (unsigned)ceil(b / df) - 1;
		if (first > last) first = last = (unsigned)lround(sqrt(a * b) / df); /* narrower than a bin */
		s_lo[k] = first;
		s_hi[k] = last < N / 2 ? last : N / 2 - 1;
	}
	done = 1;
}

/* In place, radix 2. */
static void fft(float *re, float *im)
{
	for (unsigned i = 1, j = 0; i < N; i++) {
		unsigned bit = N >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (unsigned len = 2; len <= N; len <<= 1) {
		unsigned step = N / len;
		for (unsigned i = 0; i < N; i += len)
			for (unsigned k = 0; k < len / 2; k++) {
				float wr = s_twr[k * step], wi = s_twi[k * step];
				unsigned a = i + k, b = a + len / 2;
				float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
				re[b] = re[a] - xr; im[b] = im[a] - xi;
				re[a] += xr; im[a] += xi;
			}
	}
}

void levels_spectrum(const float *mono, uint8_t *bands)
{
	static float re[N], im[N];
	setup();
	for (unsigned i = 0; i < N; i++) { re[i] = mono[i] * s_win[i]; im[i] = 0; }
	fft(re, im);
	for (unsigned k = 0; k < LEVELS_BANDS; k++) {
		float m = 0;
		for (unsigned b = s_lo[k]; b <= s_hi[k]; b++) {
			float v = re[b] * re[b] + im[b] * im[b];
			if (v > m) m = v;
		}
		/* A sine of amplitude A peaks at A * N / 4 through a Hann window. */
		double db = m > 0 ? 10.0 * log10(m) + 20.0 * log10(4.0 / N) : -FLOOR_DB;
		double v = (db + FLOOR_DB) / FLOOR_DB * 255.0;
		bands[k] = (uint8_t)(v <= 0 ? 0 : v >= 255 ? 255 : lround(v));
	}
}

/* ------------------------------------------------------------------ tap */

static uint8_t peak8(float p) { return (uint8_t)(p >= 1.0f ? 255 : lroundf(p * 255.0f)); }

void levels_enable(int on)
{
	static float drop[1024 * CH];
	on = !!on;
	if (on == s_on) return;
	if (on) {
		while (player_tap_read(drop, 1024)) {}
		memset(s_hist, 0, sizeof(s_hist));
		s_quiet = 1;
	}
	player_tap_enable(on);
	s_on = on;
}

int levels_line(char *buf, size_t cap)
{
	static float chunk[1024 * CH];
	float pk[2] = { 0, 0 };
	uint32_t got, total = 0;
	while ((got = player_tap_read(chunk, 1024)) > 0) {
		float mono[1024];
		for (uint32_t f = 0; f < got; f++) {
			float l = chunk[f * CH], r = chunk[f * CH + (CH > 1)];
			if (fabsf(l) > pk[0]) pk[0] = fabsf(l);
			if (fabsf(r) > pk[1]) pk[1] = fabsf(r);
			mono[f] = 0.5f * (l + r);
		}
		memmove(s_hist, s_hist + got, (N - got) * sizeof(*s_hist));
		memcpy(s_hist + N - got, mono, got * sizeof(*mono));
		total += got;
	}

	uint8_t b[LEVELS_BANDS] = { 0 };
	if (total) levels_spectrum(s_hist, b);
	uint8_t l = peak8(pk[0]), r = peak8(pk[1]);
	int silent = !l && !r;
	for (unsigned k = 0; k < LEVELS_BANDS && silent; k++) silent = !b[k];
	if (silent && s_quiet) return 0;
	s_quiet = silent;

	if (cap < LEVELS_LINE_MAX) return 0;
	size_t off = (size_t)snprintf(buf, cap, "levels %02x %02x ", l, r);
	for (unsigned k = 0; k < LEVELS_BANDS; k++) off += (size_t)snprintf(buf + off, cap - off, "%02x", b[k]);
	snprintf(buf + off, cap - off, "\n");
	return 1;
}

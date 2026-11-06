/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* levels: spectrum of known sines, and lines from a stand-in tap. */
#include "levels.h"
#include "player.h"
#include "mm_plugin.h"
#include "check.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CH MM_PCM_CHANNELS

/* The tap, played by the test: frames waiting to be read. */
static float s_tap[MM_PCM_RATE * CH];
static uint32_t s_tap_n, s_tap_at;
static int s_tap_on;

void player_tap_enable(int on) { s_tap_on = on; }
uint32_t player_tap_read(float *dst, uint32_t frames)
{
	uint32_t n = s_tap_n - s_tap_at < frames ? s_tap_n - s_tap_at : frames;
	memcpy(dst, s_tap + (size_t)s_tap_at * CH, (size_t)n * CH * sizeof(float));
	s_tap_at += n;
	return n;
}

/* `frames` of a sine, amplitude al on the left and ar on the right. */
static void tap_sine(double hz, float al, float ar, uint32_t frames)
{
	for (uint32_t f = 0; f < frames; f++) {
		float v = (float)sin(2.0 * M_PI * hz * f / MM_PCM_RATE);
		s_tap[f * CH] = al * v;
		if (CH > 1) s_tap[f * CH + 1] = ar * v;
	}
	s_tap_n = frames;
	s_tap_at = 0;
}

static unsigned band_of(double hz)
{
	return (unsigned)floor(LEVELS_BANDS * log(hz / 40.0) / log(16000.0 / 40.0));
}

static int spectrum(void)
{
	static float mono[LEVELS_FFT];
	uint8_t b[LEVELS_BANDS];

	for (unsigned i = 0; i < LEVELS_FFT; i++) mono[i] = (float)sin(2.0 * M_PI * 1000.0 * i / MM_PCM_RATE);
	levels_spectrum(mono, b);
	unsigned k = band_of(1000.0), top = 0;
	for (unsigned i = 1; i < LEVELS_BANDS; i++)
		if (b[i] > b[top]) top = i;
	CHECK(top == k, "a 1 kHz sine peaks in the band that holds 1 kHz");
	CHECK(b[k] >= 0xf8, "a full-scale sine reads near 0 dBFS");
	CHECK(b[band_of(150.0)] < 0x10 && b[band_of(8000.0)] < 0x10, "far bands stay at the floor");

	for (unsigned i = 0; i < LEVELS_FFT; i++) mono[i] = 0.01f * (float)sin(2.0 * M_PI * 1000.0 * i / MM_PCM_RATE);
	levels_spectrum(mono, b);
	CHECK(b[k] >= 0x68 && b[k] <= 0x76, "-40 dBFS reads 32/72 of the way up");

	for (unsigned i = 0; i < LEVELS_FFT; i++) mono[i] = (float)sin(2.0 * M_PI * 45.0 * i / MM_PCM_RATE);
	levels_spectrum(mono, b);
	CHECK(b[0] >= 0xe0, "the lowest band, narrower than a bin, still sees its tone");

	memset(mono, 0, sizeof(mono));
	levels_spectrum(mono, b);
	for (unsigned i = 0; i < LEVELS_BANDS; i++) CHECK(!b[i], "silence is all zero");
	return 0;
}

static int lines(void)
{
	char line[LEVELS_LINE_MAX];
	unsigned l, r;
	char bands[2 * LEVELS_BANDS + 1];

	tap_sine(440.0, 0.5f, 1.0f, 600);
	levels_enable(1);
	CHECK(s_tap_on && s_tap_at == s_tap_n, "enabling turns the tap on and drops what it held");
	CHECK(!levels_line(line, sizeof(line)), "nothing yet, nothing said");

	tap_sine(440.0, 0.5f, 1.0f, 2400); /* one tick of audio */
	CHECK(levels_line(line, sizeof(line)), "sound gives a line");
	CHECK(strlen(line) == 7 + 3 + 3 + 2 * LEVELS_BANDS + 1 && line[strlen(line) - 1] == '\n', "line length");
	CHECK(sscanf(line, "levels %2x %2x %64s", &l, &r, bands) == 3 && strlen(bands) == 2 * LEVELS_BANDS,
	      "levels L R bands");
	CHECK(l >= 0x7e && l <= 0x80 && r >= 0xfe, "peaks per channel");
	CHECK(s_tap_at == s_tap_n, "the tap is drained");

	CHECK(levels_line(line, sizeof(line)) && !strcmp(line, "levels 00 00 " /* then 64 zeros */
	      "0000000000000000000000000000000000000000000000000000000000000000\n"), "then silence, said once");
	CHECK(!levels_line(line, sizeof(line)), "and not again");
	tap_sine(440.0, 0.5f, 0.5f, 100);
	CHECK(levels_line(line, sizeof(line)), "sound again");

	levels_enable(0);
	CHECK(!s_tap_on, "disabling turns the tap off");
	return 0;
}

int main(void)
{
	RUN(spectrum);
	RUN(lines);
	CHECK_PASS("test-levels");
}

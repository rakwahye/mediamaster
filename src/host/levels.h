/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file levels.h
 * @brief Peak and spectrum of what is playing, for `watch levels`.
 *
 * Read from the tap (what the sink played, before volume) on the main
 * thread, once a tick. Each line:
 *
 *     levels <L> <R> <bands>
 *
 * L, R   peak of each channel since the last line: two hex digits,
 *        00..ff for 0..1 linear, clipped.
 * bands  LEVELS_BANDS values of two hex digits, low to high, log-spaced
 *        from 40 Hz to 16 kHz: the strongest bin of a LEVELS_FFT-point,
 *        Hann-windowed FFT of the newest audio (mono), 00 at -72 dBFS or
 *        below, ff at 0 dBFS (a full-scale sine).
 *
 * Silence is said once: after an all-zero line, none until there is
 * sound again (playback paused or stopped writes nothing to the tap). */

#ifndef MM_LEVELS_H
#define MM_LEVELS_H

#include <stddef.h>
#include <stdint.h>

#define LEVELS_BANDS 32
#define LEVELS_FFT 2048
#define LEVELS_LINE_MAX (16 + 2 * LEVELS_BANDS)

/** Main thread. Turns the tap on or off; turning it on first drops
 * whatever it still held from before. */
void levels_enable(int on);

/** Main thread, once a tick while enabled. Drains the tap.
 * @returns 1 with a line ("levels ...\n") in buf, 0 when there is
 * nothing to say. */
int levels_line(char *buf, size_t cap);

/** The bands of `mono` (LEVELS_FFT samples, oldest first). */
void levels_spectrum(const float *mono, uint8_t *bands);

#endif

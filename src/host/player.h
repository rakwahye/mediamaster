/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file player.h
 * @brief Playback core: rings, the active sink, one stream at a time.
 *
 * Threads:
 *   control  (IPC verbs, main-loop tick) — serialised by the player lock
 *   reader   (one per open stream)        — source->read → play ring
 *   sink     (owned by the sink plugin)   — pull: play ring → device,
 *                                           mirrors to the tap ring
 *
 * Host-owned for every source, so they all behave alike: pause, volume,
 * stop, position. A source can only lack seek and duration. */

#ifndef MM_PLAYER_H
#define MM_PLAYER_H

#include <stddef.h>
#include <stdint.h>

#define PLAYER_RING_FRAMES 65536u  /* ~1.4 s at 48 kHz */
#define PLAYER_TAP_FRAMES  131072u /* ~2.7 s */

enum player_state { PLAYER_STOPPED = 0, PLAYER_PLAYING, PLAYER_PAUSED };

/** Allocate rings, pick and start a sink (`host.sink`, an ordered list,
 * default "pipewire,null"), register the playback verbs.
 * Call after plugins are initialised. @returns 0, or -1 with no sink. */
int player_init(void);

/** Stop playback, stop the sink, drop the verbs. Call before plugins fini. */
void player_fini(void);

/** Housekeeping from the main loop: retire a stream that has finished. */
void player_tick(void);

/* Control API (also behind the verbs). err/errlen get a reason on failure. */
int player_play(const char *uri, char *err, size_t errlen);
void player_stop(void);
int player_pause(int on); /* 1 pause, 0 resume, -1 toggle; returns new paused state */
int player_seek_ms(int64_t ms, int relative, char *err, size_t errlen);
void player_set_volume(int pct); /* 0..100, perceptual curve, ramped */
int player_volume(void);
int player_state(void);
/** Frames queued in the play ring, not yet pulled by the sink. */
uint32_t player_buffered_frames(void);
/** Underruns while a stream was live (not counting after its end). */
uint64_t player_underruns(void);
/** Position of the item at the speaker, in ms. */
uint64_t player_position_ms(void);

/** Tap: what the sink actually played, before volume. Consumer side.
 * Off until enabled; while off, nothing is written and nothing counted. */
void player_tap_enable(int on);
uint32_t player_tap_read(float *dst, uint32_t frames);
uint64_t player_tap_overruns(void);

#endif

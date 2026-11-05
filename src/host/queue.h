/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file queue.h
 * @brief The play queue: an ordered list of URIs and a cursor.
 *
 * The cursor is the item playing (or last played). Positions in the API
 * are 1-based, as the user sees them.
 *
 * Removing the item under the cursor does not interrupt playback; the
 * cursor is then "between" items, and `next` plays the one that followed.
 *
 * Gapless: when the player asks, the queue preloads the next item that
 * opens; the player crosses into it without a gap and the cursor moves
 * when the new item reaches the speaker. If nothing was preloaded in time
 * the queue starts the next item itself, skipping ones that fail to open.
 * At the end of the queue playback stops and the cursor stays on the last
 * item.
 *
 * Owns the verbs play, next, prev and queue. */

#ifndef MM_QUEUE_H
#define MM_QUEUE_H

#include <stddef.h>

int queue_init(void);
void queue_fini(void);

/** Append. @returns new 1-based position, or -1. */
long queue_add(const char *uri);
/** Insert right after the cursor and play it now. */
int queue_play_uri(const char *uri, char *err, size_t errlen);
/** Play position `pos` (1-based); 0 = the cursor, or the first item. */
int queue_play_at(long pos, char *err, size_t errlen);
int queue_next(char *err, size_t errlen);
int queue_prev(char *err, size_t errlen);
int queue_remove(long pos);
void queue_clear(void);
size_t queue_length(void);
/** Cursor as a 1-based position, 0 when there is none. */
long queue_cursor(void);

/** Main-loop step: run player_tick, follow gapless advances, fall back to
 * starting the next item when playback stopped, and preload the next item
 * when the player asks. @returns the player's event bits. */
int queue_tick(void);

/** The current stream played to its end with nothing preloaded: start the
 * next playable item (with a gap). queue_tick calls this. */
void queue_on_finished(void);

/** One line per item, "> " marking the cursor. */
void queue_list(char *buf, size_t cap);

#endif

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file watch.h
 * @brief `watch`: a connection kept open for events.
 *
 *     watch [state|track|volume|seek|queue|meta]...
 *
 * No names means all of them. The reply is "ok watching <names>\n"; then,
 * after each main-loop tick in which any of them fired, one line naming
 * those that did, in that order:
 *
 *     state track
 *
 * Changes are found by comparing state once a tick (50 ms), so several
 * in one tick share a line, and one undone within a tick (pause and
 * resume) is not seen. The line says what changed, not to what: a
 * client reads `status` or
 * `queue list` for that. A client that hangs up, or stops reading until
 * its socket buffer is full, is dropped.
 *
 * The IPC thread hands connections over (watch_adopt); the main thread
 * writes to them (watch_tick). */

#ifndef MM_WATCH_H
#define MM_WATCH_H

#include <stdint.h>

#define WATCH_MAX 16

/** Register `watch` (for help; in-process it is refused) and the IPC
 * hand-over. @returns 0, or -1. */
int watch_init(void);

/** Close every watcher. Call after ipc_stop. */
void watch_fini(void);

/** IPC thread: take connection `fd` for `watch argv[1..]`, or refuse it.
 * Writes the ok/err reply. @returns 1 if fd is kept, 0 if the caller
 * should close it. */
int watch_adopt(int fd, int argc, char *const *argv);

/** Main thread, after events_poll: tell watchers what fired (`ev`,
 * MM_EV_* bits), and drop those that went away. */
void watch_tick(uint32_t ev);

#endif

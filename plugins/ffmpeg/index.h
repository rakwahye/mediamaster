/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file index.h
 * @brief The music folder, indexed, so a search never walks the disk.
 *
 * The index lives in a file between runs. Opening it loads that file and
 * starts a scan in the background that brings it up to date the cheap
 * way: a folder whose modification time is unchanged keeps its saved
 * entries after one stat, without being read; only a changed or new
 * folder is read. (A folder's time changes when an entry is added,
 * removed or renamed in it.) Searches are answered from whichever index
 * is current, so they are instant even while a slow drive is scanned. */

#ifndef FF_INDEX_H
#define FF_INDEX_H

#include "mm_plugin.h"

struct ffidx;

/** Is `name` an audio file, by its extension? */
int ffidx_audio_name(const char *name);

/** Load `saved` (may not exist yet) for the folder `root` and start a
 * scan. @returns the index, or NULL if out of memory. */
struct ffidx *ffidx_open(const struct mm_host *h, const char *root, const char *saved);

/** Seconds between checkpoints of a running scan (default 15; 0 makes
 * one at every folder, for tests). */
void ffidx_checkpoint_every(struct ffidx *x, int seconds);

/** Scan again (on config reload). A scan already running carries on. */
void ffidx_rescan(struct ffidx *x);

/** emit(ctx, item) for each indexed audio file whose path below the root
 * holds every word, in any case and order, until emit says stop. */
void ffidx_search(struct ffidx *x, const char *words, mm_item_fn emit, void *ctx);

struct ffidx_stats {
	long dirs, files;  /* in the current index */
	long dirs_read;    /* folders the last scan read, rather than reused */
	int scanning;      /* a scan is running */
	long scans;        /* scans completed */
};
void ffidx_stats(struct ffidx *x, struct ffidx_stats *out);

/** Stop any scan and free. */
void ffidx_close(struct ffidx *x);

#endif

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file index.h
 * @brief The music folder, indexed with tags, so nothing waits on the disk.
 *
 * Kept in a file between runs. Opening loads it and starts a scan in the
 * background that brings it up to date the cheap way:
 *
 *   1. Folders: one whose modification time is unchanged keeps its saved
 *      entries without being read; only changed and new folders are read.
 *      Each file is stat'ed, so a retagged file (same folder, new time
 *      or size) is noticed.
 *   2. Tags: only new and changed files are probed (ffprobe).
 *
 * Both checkpoint as they go: the index is current and saved every few
 * seconds, so a first scan of a slow drive is searchable while it runs
 * and is never lost to a restart. Queries read whichever index is
 * current, from memory. */

#ifndef LIB_INDEX_H
#define LIB_INDEX_H

#include "mm_plugin.h"

struct libidx;

/** One indexed track, as a query sees it. Valid during the callback. */
struct libidx_track {
	const char *path;        /* absolute */
	const char *rel;         /* below the root */
	const char *name;        /* file name */
	const char *tags;        /* "key=value\n" lines, "" if none */
	int probed;              /* tags and length are known */
	uint64_t duration_ms;
};

/** Is `name` an audio file, by its extension? */
int libidx_audio_name(const char *name);

/** Load `saved` (may not exist yet) for the folder `root` and start a
 * scan; tags are read with `ffprobe`. @returns NULL if out of memory. */
struct libidx *libidx_open(const struct mm_host *h, const char *root, const char *saved, const char *ffprobe);

/** Scan again. A scan already running carries on. */
void libidx_rescan(struct libidx *x);

/** fn(ctx, track) for every track, folders in path order and files in
 * natural order within one, until fn returns nonzero. Holds the index's
 * lock: fn must be quick and must not call back into the index. */
void libidx_each(struct libidx *x, int (*fn)(void *ctx, const struct libidx_track *t), void *ctx);

/** The value of `key` in tag lines, into out ("" if absent). */
void libidx_tag(const char *tags, const char *key, char *out, size_t cap);

struct libidx_stats {
	long dirs, files, probed;  /* in the current index */
	long dirs_read, probes;    /* what the last scan read and probed */
	int scanning;
	long scans;                /* scans completed */
};
void libidx_stats(struct libidx *x, struct libidx_stats *out);

/** Seconds between checkpoints (default 15; 0 = at every step, for tests). */
void libidx_checkpoint_every(struct libidx *x, int seconds);

/** Stop any scan, saving its progress, and free. */
void libidx_close(struct libidx *x);

#endif

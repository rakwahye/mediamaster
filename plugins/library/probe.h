/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file probe.h
 * @brief One file's tags and length, by running ffprobe. */

#ifndef LIB_PROBE_H
#define LIB_PROBE_H

#include <stddef.h>
#include <stdint.h>

/** Probe `path` with `ffprobe` (a binary name or path), giving up after
 * `timeout_ms`. Tags go into `tags` as "key=value\n" lines in the host's
 * names (title, artist, album, albumartist, tracknumber, discnumber, date,
 * genre); each once, the first found; tabs in values become
 * spaces. @returns the length in ms (0 if unknown), or -1 if ffprobe could
 * not be run at all. */
long long probe_file(const char *ffprobe, const char *path, long timeout_ms, char *tags, size_t cap);

#endif

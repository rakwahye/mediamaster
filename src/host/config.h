/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file config.h
 * @brief Flat key=value config.
 *
 * Keys are "<owner>.<key>": "host.*" for the host, "<plugin>.*" for each
 * plugin. The config has no schema; owners read what they need.
 *
 * File format: one `key = value` per line. `#` starts a comment at line
 * start or after whitespace. A value in double quotes is taken verbatim
 * (keeps `#` and surrounding spaces). Later lines win.
 *
 * Overrides (`-o key=value`) are applied on top of the file at every load
 * and survive reloads.
 *
 * Lifetime: a string returned by config_get stays valid until config_free.
 * Reload publishes a new table and keeps the old ones, so a reader on
 * another thread never sees freed memory. Reloads are rare; this is cheap. */

#ifndef MM_CONFIG_H
#define MM_CONFIG_H

/** Load `path` and make it current. A missing file is not an error: the
 * table is just empty (plus overrides). @returns 0, or -1 on a read error. */
int config_load(const char *path);

/** Re-read the last loaded path. */
int config_reload(void);

/** Add a sticky override "key=value". @returns 0, -1 if malformed. */
int config_override(const char *kv);

/** Value for `key`, or NULL. Thread-safe. */
const char *config_get(const char *key);

const char *config_get_str(const char *key, const char *def);
long config_get_long(const char *key, long def);
int config_get_bool(const char *key, int def);

/** Path last loaded, or "" before any load. */
const char *config_path(void);

void config_free(void);

#endif

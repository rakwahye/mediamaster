/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file loader.h
 * @brief Plugin loading and lifecycle.
 *
 * Plugins arrive two ways, through the same struct: static builtins
 * (loader_add with dl == NULL) and shared objects found on the plugin
 * path. Names are unique; the first to claim a name keeps it.
 *
 * A plugin that is refused (bad ABI, bad name, duplicate) is not loaded.
 * A plugin listed in `host.disable` is kept but never initialised. A
 * plugin whose init fails is marked failed and the daemon carries on:
 * one broken plugin must not take the player down. */

#ifndef MM_LOADER_H
#define MM_LOADER_H

#include "mm_plugin.h"
#include <stddef.h>

#define MM_MAX_PLUGINS 64

enum mm_plugin_state {
	MM_PLUGIN_LOADED = 0,
	MM_PLUGIN_ACTIVE,
	MM_PLUGIN_FAILED,
	MM_PLUGIN_DISABLED
};

/** Register a plugin. `dl` is the dlopen handle (closed by the loader at
 * fini) or NULL for a builtin. `origin` is a path or "builtin".
 * @returns 0 if accepted, -1 if refused (caller keeps ownership of dl). */
int loader_add(const struct mm_plugin *p, void *dl, const char *origin);

/** Scan a colon-separated list of directories for *.so, in name order. */
void loader_scan(const char *dirs);

/** Init every loaded, non-disabled plugin, in load order. */
void loader_init_all(void);

/** Call `reload` on every active plugin. */
void loader_reload_all(void);

/** Fini active plugins in reverse order, drop their verbs, dlclose. */
void loader_fini_all(void);

/** State of plugin `name`, or -1 if unknown. */
int loader_state(const char *name);

size_t loader_count(void);

/** One line per plugin: name version state ifaces= attrs= caps= origin. */
void loader_describe(char *buf, size_t cap);

#endif

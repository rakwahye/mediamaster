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
/** Shut down the plugins that neither play nor output (control, lists:
 * MPRIS, the library) while the player still runs, so none of their
 * threads can call into a player being torn down. loader_fini_all then
 * only unloads them. */
void loader_fini_controls(void);

/** Active plugin with source ops claiming URI scheme `scheme`.
 * @returns 0 and fills *p / *priv, or -1. */
int loader_find_source(const char *scheme, const struct mm_plugin **p, void **priv);

/** Active plugin with catalog ops claiming `scheme`. @returns 0 or -1. */
int loader_find_catalog(const char *scheme, const struct mm_plugin **p, void **priv);

/** Active plugin `name` with a catalog match op. @returns 0 or -1. */
int loader_find_match(const char *name, const struct mm_plugin **p, void **priv);

/** Active plugin `name` with a catalog radio op. @returns 0 or -1. */
int loader_find_radio(const char *name, const struct mm_plugin **p, void **priv);

/** fn(ctx, plugin name) for each active plugin with a radio op, in load
 * order. */
void loader_each_radio(void (*fn)(void *ctx, const char *name), void *ctx);

/** fn(ctx, scheme, plugin name) for each active catalog plugin, in load
 * order, with the first scheme it claims. */
void loader_each_catalog(void (*fn)(void *ctx, const char *scheme, const char *name), void *ctx);

/** Active plugin named `name` that has sink ops. @returns 0 or -1. */
int loader_find_sink(const char *name, const struct mm_plugin **p, void **priv);

/** State of plugin `name`, or -1 if unknown. */
int loader_state(const char *name);

size_t loader_count(void);

/** One line per plugin: name version state ifaces= attrs= caps= origin. */
void loader_describe(char *buf, size_t cap);

#endif

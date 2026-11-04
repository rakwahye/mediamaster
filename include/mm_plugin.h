/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file mm_plugin.h
 * @brief mediamaster plugin ABI.
 *
 * The only header a plugin sees. Strict C99, no host internals.
 *
 * A plugin is a shared object exporting one symbol, `mm_plugin_entry`,
 * that returns a static `struct mm_plugin`. The plugin implements any
 * subset of the interfaces below by pointing at an ops table; NULL means
 * "not provided". Nothing about a plugin is classified by where it lives.
 * `attrs` carry the policy facts the host cares about instead.
 *
 * An ops struct that is only forward-declared here is not part of the
 * current ABI yet: leave it NULL.
 *
 * Threading: `init`, `fini` and `reload` run on the host main thread.
 * Verb handlers run on the IPC thread. A plugin that shares state between
 * the two guards it itself. */

#ifndef MM_PLUGIN_H
#define MM_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MM_ABI 1

/** Policy attributes. The host uses these for the offline switch, rate
 * budgets and credential handling, never for dispatch. */
enum mm_attr {
	MM_ATTR_NETWORK        = 1u << 0, /* talks to the network */
	MM_ATTR_SPAWNS         = 1u << 1, /* runs external binaries */
	MM_ATTR_NEEDS_LOGIN    = 1u << 2, /* holds user credentials */
	MM_ATTR_RATE_LIMITED   = 1u << 3, /* upstream enforces a request rate */
	MM_ATTR_UNOFFICIAL_API = 1u << 4  /* upstream may break without notice */
};

enum mm_log_level {
	MM_LOG_ERROR = 0,
	MM_LOG_WARN  = 1,
	MM_LOG_INFO  = 2,
	MM_LOG_DEBUG = 3
};

/** Verb handler return codes. For MM_OK / MM_ERR the host frames the reply
 * as "ok <reply>\n" / "err <reply>\n". MM_REPLY_RAW sends `reply` verbatim,
 * for multi-line output. */
enum mm_reply {
	MM_OK        = 0,
	MM_ERR       = 1,
	MM_REPLY_RAW = 2
};

/** One IPC command as a verb handler sees it. argv[0] is the verb. Words
 * are byte-exact: they may hold spaces or newlines. `body` is whatever the
 * client sent after the last word in the same frame. */
struct mm_verb_ctx {
	int argc;
	char *const *argv;
	const char *body;
	size_t body_len;
	char *reply;      /* handler writes reply text here (NUL-terminated) */
	size_t reply_len; /* capacity of `reply` */
};

typedef int (*mm_verb_fn)(void *priv, struct mm_verb_ctx *c);

/** Services the host offers a plugin. Each plugin gets its own copy, so
 * the host knows who is calling. Check `size` before touching a field
 * added after ABI 1. */
struct mm_host {
	uint32_t abi;
	uint32_t size; /* sizeof(struct mm_host) as the host built it */

	/** Log tagged with the plugin name. printf-style. */
	void (*log)(const struct mm_host *h, int level, const char *fmt, ...);

	/** Config value for "<plugin>.<key>", or NULL if unset. The pointer
	 * stays valid until the plugin's `fini`; re-read it after `reload`. */
	const char *(*config_get)(const struct mm_host *h, const char *key);

	/** Add an IPC verb. `help` is "ARGS\tdescription" (ARGS may be empty).
	 * Fails (nonzero) if the verb is taken. Verbs are removed for the
	 * plugin automatically at `fini`. Call from `init` only. */
	int (*register_verb)(const struct mm_host *h, const char *verb,
	                     const char *help, mm_verb_fn fn, void *priv);

	void *ctx; /* host-private */
};

/* Interface ops tables. Forward-declared ones are not defined yet. */
struct mm_source_ops;
struct mm_sink_ops;
struct mm_catalog_ops;
struct mm_identify_ops;
struct mm_enrich_ops;
struct mm_observer_ops;
struct mm_acquire_ops;
struct mm_store_ops;
struct mm_orch_ops;

struct mm_plugin {
	uint32_t abi;   /* MM_ABI the plugin was built against */
	uint32_t attrs; /* enum mm_attr bits */
	const char *name;    /* unique, no '.' or whitespace; config prefix */
	const char *version;
	const char *description;
	const char *schemes; /* URI schemes claimed, comma-separated, or NULL */

	/** Required: set *priv. Return 0 on success. On failure the plugin is
	 * marked failed and `fini` is not called. */
	int (*init)(const struct mm_host *h, void **priv);
	void (*fini)(void *priv);
	/** Optional: config was re-read. */
	void (*reload)(void *priv);
	/** Optional: live capability mask. May shrink at runtime. */
	uint32_t (*caps)(void *priv);

	const struct mm_source_ops   *source;
	const struct mm_sink_ops     *sink;
	const struct mm_catalog_ops  *catalog;
	const struct mm_identify_ops *identify;
	const struct mm_enrich_ops   *enrich;
	const struct mm_observer_ops *observer;
	const struct mm_acquire_ops  *acquire;
	const struct mm_store_ops    *store;
	const struct mm_orch_ops     *orchestrator;
};

typedef const struct mm_plugin *(*mm_plugin_entry_fn)(void);

/** MM_PLUGIN_DEFINE(id) { return &my_plugin; }
 *
 * Shared-object build: defines the exported `mm_plugin_entry`.
 * Static build (-DMM_PLUGIN_STATIC): defines `mm_static_entry_<id>` so
 * several plugins can be linked into one binary. */
#if defined(MM_PLUGIN_STATIC)
# define MM_PLUGIN_DEFINE(id) \
	const struct mm_plugin *mm_static_entry_##id(void); \
	const struct mm_plugin *mm_static_entry_##id(void)
#else
# if defined(__GNUC__)
#  define MM_EXPORT __attribute__((visibility("default")))
# else
#  define MM_EXPORT
# endif
MM_EXPORT const struct mm_plugin *mm_plugin_entry(void);
# define MM_PLUGIN_DEFINE(id) MM_EXPORT const struct mm_plugin *mm_plugin_entry(void)
#endif

#ifdef __cplusplus
}
#endif

#endif

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

/** Host events, delivered on the host main thread (within ~50 ms). */
enum mm_event {
	MM_EV_STATE  = 1u << 0, /* playing / paused / stopped changed */
	MM_EV_TRACK  = 1u << 1, /* a different item reached the speaker */
	MM_EV_VOLUME = 1u << 2,
	MM_EV_SEEK   = 1u << 3, /* position jumped (seek) */
	MM_EV_QUEUE  = 1u << 4, /* queue contents or cursor changed */
	MM_EV_META   = 1u << 5  /* the item at the speaker learned its duration etc. */
};

typedef void (*mm_event_fn)(void *priv, uint32_t events);

struct mm_stream_info; /* defined with the source interface below */

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

	/** Run a command in-process, exactly as an mmctl client would
	 * (argv[0] is the verb). `reply` gets the reply text without the
	 * ok/err framing. Callable from any plugin thread, but not from inside
	 * a verb handler. @returns MM_OK, MM_ERR or MM_REPLY_RAW. */
	int (*command)(const struct mm_host *h, int argc, const char *const *argv,
	               char *reply, size_t reply_len);

	/** Call cb(priv, events) on the host main thread whenever any event in
	 * `mask` (enum mm_event) happens. Keep cb short; hand work to your own
	 * thread. Call from `init` only; removed automatically at `fini`. */
	int (*subscribe)(const struct mm_host *h, uint32_t mask, mm_event_fn cb, void *priv);

	void *ctx; /* host-private */

	/* ---- fields below were added after the first plugins were built;
	 * check `size` before using them. */

	/** A source that only learns a stream's duration or seekability after
	 * `open` (a download completing, a header arriving) reports it here.
	 * `stream` is the handle `open` returned; `info` replaces what `open`
	 * filled in. Any thread, but never after `close` returned for that
	 * stream. */
	void (*stream_update)(const struct mm_host *h, void *stream, const struct mm_stream_info *info);
};

/** True if host `h` provides `field` (it was built with it). */
#define MM_HOST_HAS(h, field) \
	((h)->size >= offsetof(struct mm_host, field) + sizeof((h)->field) && (h)->field)

/* ------------------------------------------------------------------ PCM
 *
 * Between sources and sinks audio is always interleaved float32 at
 * MM_PCM_RATE with MM_PCM_CHANNELS channels. Conversion happens at the
 * edges, inside the plugins. */

#define MM_PCM_RATE     48000u
#define MM_PCM_CHANNELS 2u

/* ------------------------------------------------------------------ source */

enum mm_stream_flags {
	MM_STREAM_SEEKABLE = 1u << 0,
	MM_STREAM_LIVE     = 1u << 1  /* no end, no duration (radio, generators) */
};

struct mm_stream_info {
	uint64_t duration_ms; /* 0 = unknown */
	uint32_t flags;       /* enum mm_stream_flags */
};

/** Turns a URI into PCM. The host owns the thread that calls `read` and
 * the ring the PCM goes into; a source only produces samples.
 *
 * `open` and `close` run on host control threads; `read` and `seek` on
 * the host's reader thread, never concurrently for one stream.
 * `interrupt` may come from any thread while `read` is blocked. */
struct mm_source_ops {
	/** Open `uri` (one of the plugin's schemes). Fill *stream and *info.
	 * @returns 0, or nonzero with nothing to close. */
	int (*open)(void *priv, const char *uri, void **stream, struct mm_stream_info *info);

	/** Read up to `frames` frames into dst. May return fewer; blocks until
	 * at least one frame or the end. @returns frames read, 0 at end (or
	 * after `interrupt`), negative on error. */
	long (*read)(void *stream, float *dst, uint32_t frames);

	/** Optional (needs MM_STREAM_SEEKABLE). Reposition to `ms`; clears a
	 * pending interrupt. @returns 0 on success. */
	int (*seek)(void *stream, uint64_t ms);

	/** Optional. Make a blocked or the next `read` return 0 promptly. */
	void (*interrupt)(void *stream);

	void (*close)(void *stream);
};

/* ------------------------------------------------------------------ sink */

/** Host-owned PCM supplier handed to a sink. Fills `frames` frames into
 * dst, always completely (silence when there is nothing to play).
 * Realtime-safe: no locks, no allocation, no syscalls. */
typedef void (*mm_pull_fn)(void *ctx, float *dst, uint32_t frames);

/** Plays what the host's pull function supplies. Volume, pause, mixing
 * and the identification tap all live in the host, so a sink is only a
 * clock and a device. */
struct mm_sink_ops {
	/** Open the device and start calling `pull(ctx, ...)` from the sink's
	 * own (preferably realtime) thread. @returns 0 on success. */
	int (*start)(void *priv, mm_pull_fn pull, void *ctx);
	/** Stop calling pull and release the device. After return, pull is
	 * never called again. */
	void (*stop)(void *priv);
};

/* Interface ops tables not defined yet: leave NULL. */
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

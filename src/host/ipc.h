/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file ipc.h
 * @brief Command socket and verb registry.
 *
 * Wire format, one command per connection:
 *
 *     <verb> argc=N\n
 *     <arg0>\0<arg1>\0...<argN-1>\0
 *     [body]
 *
 * arg0 is the verb again. NUL separates words, so a word may carry spaces
 * or newlines unescaped. Bytes after the Nth NUL that arrived with the
 * frame are the body. A header without `argc=` is split on whitespace
 * (handy for `socat`/`nc`).
 *
 * The daemon writes one reply and closes: "ok ...\n", "err ...\n", or raw
 * multi-line text for verbs that ask for it.
 *
 * Handlers run one at a time: on the IPC thread for socket clients, or on
 * the caller's thread for in-process commands (ipc_dispatch). Registration and
 * unregistration happen on the main thread while the listener is stopped
 * (startup, shutdown); the registry is locked regardless. */

#ifndef MM_IPC_H
#define MM_IPC_H

#include "mm_plugin.h"

#define IPC_MAX_ARGS 64
#define IPC_FRAME_MAX 65536
#define IPC_REPLY_MAX (1 << 20) /* a long playlist through browse */

/** Register `verb` for `owner`. `help` is "ARGS\tdescription".
 * @returns 0, or -1 if taken, reserved, malformed, or the table is full. */
int ipc_register(const char *owner, const char *verb, const char *help,
                 mm_verb_fn fn, void *priv);

/** Drop every verb `owner` registered. */
void ipc_unregister_owner(const char *owner);

/** Run a verb in-process, exactly as a client would. `reply` gets the
 * handler's text, without the ok/err framing (trailing newlines trimmed
 * unless raw). Handlers are serialised with socket clients. Must not be
 * called from inside a verb handler (it would deadlock).
 * @returns MM_OK, MM_ERR or MM_REPLY_RAW. */
int ipc_dispatch(int argc, char *const *argv, const char *body, size_t body_len,
                 char *reply, size_t reply_len);

/** Hands a client's connection to `fn` instead of answering it once.
 * fn runs on the IPC thread, writes its own reply, and @returns 1 to keep
 * the fd (it then owns it) or 0 for the listener to close it. */
typedef int (*ipc_adopt_fn)(int fd, int argc, char *const *argv);

/** Connections whose verb is `verb` go to `fn` (one such verb; NULL
 * clears it). The verb should also be registered, for help. */
void ipc_set_adopt(const char *verb, ipc_adopt_fn fn);

/** @returns 1 if `verb` is registered. */
int ipc_has_verb(const char *verb);

/** Bind `path` (0600) and start the listener thread. Refuses if another
 * process is already answering on `path`; removes a stale socket file.
 * @returns 0 on success. */
int ipc_start(const char *path);

/** Stop the listener, join it, remove the socket file. Idempotent. */
void ipc_stop(void);

#endif

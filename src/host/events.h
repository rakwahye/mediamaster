/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file events.h
 * @brief Host event detection and delivery to subscribed plugins.
 *
 * The main loop calls events_poll() after each tick. It compares a
 * snapshot of player and queue state with the previous one and calls
 * every subscriber whose mask matches what changed. Main thread only,
 * apart from subscribe/unsubscribe, which are locked. */

#ifndef MM_EVENTS_H
#define MM_EVENTS_H

#include "mm_plugin.h"

int events_subscribe(const char *owner, uint32_t mask, mm_event_fn cb, void *priv);
void events_unsubscribe_owner(const char *owner);

/** Detect changes since the last call and deliver them.
 * @returns the event bits that fired. */
uint32_t events_poll(void);

#endif

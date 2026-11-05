/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file radio.h
 * @brief When the queue is down to its last item, more like it. */

#ifndef MM_RADIO_H
#define MM_RADIO_H

/** Register `radio`. */
int radio_init(void);
/** Main loop: start a fetch when the current item is the last. */
void radio_tick(void);
/** Wait for a fetch in flight. */
void radio_fini(void);

#endif

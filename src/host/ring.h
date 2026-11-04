/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file ring.h
 * @brief Single-producer single-consumer PCM ring and marker queue.
 *
 * Free-running 64-bit positions, slot = pos & (cap - 1). The producer
 * alone writes `head`, the consumer alone writes `tail`, so both sides are
 * lock-free and wait-free. Neither side ever blocks; what to do when the
 * ring is full or empty is the owner's policy:
 *
 *   play ring: producer waits for space (a player must not drop audio),
 *              consumer outputs silence and counts an underrun.
 *   tap ring:  producer drops what does not fit and counts an overrun.
 *
 * The marker queue rides alongside a ring: the producer stamps an event
 * (track start, track end) at a ring position, and the consumer acts on it
 * exactly when playback reaches that frame. */

#ifndef MM_RING_H
#define MM_RING_H

#include <stdatomic.h>
#include <stdint.h>

struct mm_ring {
	float *buf;
	uint32_t cap;      /* frames, power of two */
	uint32_t channels;
	_Alignas(64) atomic_uint_least64_t head; /* producer */
	_Alignas(64) atomic_uint_least64_t tail; /* consumer */
};

/** @returns 0, or -1 if `frames` is not a power of two or allocation fails. */
int ring_init(struct mm_ring *r, uint32_t frames, uint32_t channels);
/** Same, with both positions starting at `start` (tests the 64-bit wrap). */
int ring_init_at(struct mm_ring *r, uint32_t frames, uint32_t channels, uint64_t start);
void ring_free(struct mm_ring *r);

/* Producer side. */
uint32_t ring_write(struct mm_ring *r, const float *src, uint32_t frames); /* writes what fits */
uint32_t ring_space(const struct mm_ring *r);
uint64_t ring_head(const struct mm_ring *r);

/* Consumer side. */
uint32_t ring_read(struct mm_ring *r, float *dst, uint32_t frames);        /* reads what is there */
uint64_t ring_tail(const struct mm_ring *r);
/** Discard up to absolute position `pos` (clamped to head). */
void ring_skip_to(struct mm_ring *r, uint64_t pos);

/* Either side; a snapshot. */
uint32_t ring_fill(const struct mm_ring *r);

/* ------------------------------------------------------------------ markers */

enum mm_mark_kind {
	MM_MARK_START = 1, /* item begins playing here, at `offset` frames into it */
	MM_MARK_END   = 2  /* item has fully played once the consumer gets here */
};

struct mm_mark {
	uint64_t pos;    /* absolute ring position */
	uint64_t offset; /* START: frames into the item (non-zero after a seek) */
	uint32_t item;
	uint32_t kind;
};

#define MM_MARKQ_CAP 64u

struct mm_markq {
	struct mm_mark q[MM_MARKQ_CAP];
	_Alignas(64) atomic_uint_least64_t head;
	_Alignas(64) atomic_uint_least64_t tail;
};

void markq_init(struct mm_markq *m);
/** Producer. @returns 0, or -1 if full. Positions must be non-decreasing. */
int markq_push(struct mm_markq *m, const struct mm_mark *mk);
/** Consumer: oldest marker without removing it. @returns 1 if any. */
int markq_peek(const struct mm_markq *m, struct mm_mark *out);
void markq_pop(struct mm_markq *m);

#endif

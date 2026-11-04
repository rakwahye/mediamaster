/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file ring.c
 * @brief SPSC PCM ring and marker queue.
 *
 * Ordering: a side publishes its counter with release after touching the
 * slots, and reads the other side's counter with acquire before touching
 * them. Its own counter it reads relaxed. */

#include "ring.h"
#include <stdlib.h>
#include <string.h>

static int pow2(uint32_t v) { return v && !(v & (v - 1)); }

int ring_init_at(struct mm_ring *r, uint32_t frames, uint32_t channels, uint64_t start)
{
	if (!pow2(frames) || !channels) return -1;
	r->buf = calloc((size_t)frames * channels, sizeof(float));
	if (!r->buf) return -1;
	r->cap = frames;
	r->channels = channels;
	atomic_init(&r->head, start);
	atomic_init(&r->tail, start);
	return 0;
}

int ring_init(struct mm_ring *r, uint32_t frames, uint32_t channels)
{
	return ring_init_at(r, frames, channels, 0);
}

void ring_free(struct mm_ring *r)
{
	free(r->buf);
	r->buf = NULL;
	r->cap = 0;
}

static uint32_t used(uint64_t head, uint64_t tail) { return (uint32_t)(head - tail); }

uint32_t ring_space(const struct mm_ring *r)
{
	uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	return r->cap - used(h, t);
}

uint32_t ring_fill(const struct mm_ring *r)
{
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	uint32_t u = used(h, t);
	return u > r->cap ? r->cap : u;
}

uint64_t ring_head(const struct mm_ring *r) { return atomic_load_explicit(&r->head, memory_order_acquire); }
uint64_t ring_tail(const struct mm_ring *r) { return atomic_load_explicit(&r->tail, memory_order_acquire); }

/* Copy `frames` frames between the linear buffer and the ring at `pos`,
 * splitting at the wrap point. */
static void copy_in(struct mm_ring *r, uint64_t pos, const float *src, uint32_t frames)
{
	uint32_t at = (uint32_t)(pos & (r->cap - 1));
	uint32_t first = r->cap - at < frames ? r->cap - at : frames;
	size_t fs = (size_t)r->channels * sizeof(float);
	memcpy(r->buf + (size_t)at * r->channels, src, first * fs);
	if (frames > first) memcpy(r->buf, src + (size_t)first * r->channels, (frames - first) * fs);
}

static void copy_out(const struct mm_ring *r, uint64_t pos, float *dst, uint32_t frames)
{
	uint32_t at = (uint32_t)(pos & (r->cap - 1));
	uint32_t first = r->cap - at < frames ? r->cap - at : frames;
	size_t fs = (size_t)r->channels * sizeof(float);
	memcpy(dst, r->buf + (size_t)at * r->channels, first * fs);
	if (frames > first) memcpy(dst + (size_t)first * r->channels, r->buf, (frames - first) * fs);
}

uint32_t ring_write(struct mm_ring *r, const float *src, uint32_t frames)
{
	uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	uint32_t space = r->cap - used(h, t);
	if (frames > space) frames = space;
	if (!frames) return 0;
	copy_in(r, h, src, frames);
	atomic_store_explicit(&r->head, h + frames, memory_order_release);
	return frames;
}

uint32_t ring_read(struct mm_ring *r, float *dst, uint32_t frames)
{
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	uint32_t avail = used(h, t);
	if (frames > avail) frames = avail;
	if (!frames) return 0;
	copy_out(r, t, dst, frames);
	atomic_store_explicit(&r->tail, t + frames, memory_order_release);
	return frames;
}

void ring_skip_to(struct mm_ring *r, uint64_t pos)
{
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	if ((int64_t)(pos - h) > 0) pos = h;     /* never past what was written */
	if ((int64_t)(pos - t) <= 0) return;     /* already beyond */
	atomic_store_explicit(&r->tail, pos, memory_order_release);
}

/* ------------------------------------------------------------------ markers */

void markq_init(struct mm_markq *m)
{
	memset(m->q, 0, sizeof(m->q));
	atomic_init(&m->head, 0);
	atomic_init(&m->tail, 0);
}

int markq_push(struct mm_markq *m, const struct mm_mark *mk)
{
	uint64_t h = atomic_load_explicit(&m->head, memory_order_relaxed);
	uint64_t t = atomic_load_explicit(&m->tail, memory_order_acquire);
	if (h - t >= MM_MARKQ_CAP) return -1;
	m->q[h % MM_MARKQ_CAP] = *mk;
	atomic_store_explicit(&m->head, h + 1, memory_order_release);
	return 0;
}

int markq_peek(const struct mm_markq *m, struct mm_mark *out)
{
	uint64_t t = atomic_load_explicit(&m->tail, memory_order_relaxed);
	uint64_t h = atomic_load_explicit(&m->head, memory_order_acquire);
	if (h == t) return 0;
	*out = m->q[t % MM_MARKQ_CAP];
	return 1;
}

void markq_pop(struct mm_markq *m)
{
	uint64_t t = atomic_load_explicit(&m->tail, memory_order_relaxed);
	uint64_t h = atomic_load_explicit(&m->head, memory_order_acquire);
	if (h != t) atomic_store_explicit(&m->tail, t + 1, memory_order_release);
}

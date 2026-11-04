/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ring.h"
#include "check.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int basics(void)
{
	struct mm_ring r;
	CHECK(ring_init(&r, 1000, 2) != 0, "a capacity that is not a power of two is refused");
	CHECK(ring_init(&r, 8, 2) == 0, "a power-of-two ring initialises");
	CHECK(ring_fill(&r) == 0 && ring_space(&r) == 8, "a new ring is empty");

	float in[20], out[20];
	for (int i = 0; i < 20; i++) in[i] = (float)i;
	CHECK(ring_write(&r, in, 5) == 5, "a write that fits is taken whole");
	CHECK(ring_write(&r, in + 10, 5) == 3, "a write past capacity takes only what fits");
	CHECK(ring_fill(&r) == 8 && ring_space(&r) == 0, "the ring reports itself full");
	CHECK(ring_write(&r, in, 1) == 0, "a full ring takes nothing");

	CHECK(ring_read(&r, out, 6) == 6, "a read takes what was asked when available");
	CHECK(!memcmp(out, in, 10 * sizeof(float)) && out[10] == 10 && out[11] == 11, "frames come out in order, channels interleaved");
	CHECK(ring_read(&r, out, 10) == 2, "a read takes only what is there");
	CHECK(out[0] == 12 && out[3] == 15, "the partial write's frames follow");
	CHECK(ring_read(&r, out, 1) == 0, "an empty ring gives nothing");
	ring_free(&r);
	return 0;
}

static int wrap64(void)
{
	struct mm_ring r;
	CHECK(ring_init_at(&r, 64, 2, UINT64_MAX - 40) == 0, "a ring can start near the 64-bit limit");
	float in[2 * 200], out[2 * 200];
	for (int i = 0; i < 400; i++) in[i] = (float)i;
	uint32_t w = 0, rd = 0;
	while (rd < 200) {
		w += ring_write(&r, in + 2 * w, 200 - w > 50 ? 50 : 200 - w);
		rd += ring_read(&r, out + 2 * rd, 37);
	}
	CHECK(!memcmp(in, out, sizeof(in)), "data survives the head and tail counters wrapping past 2^64");
	CHECK(ring_head(&r) < 1000, "the counters really did wrap");
	ring_free(&r);
	return 0;
}

static int skip(void)
{
	struct mm_ring r;
	float in[32] = { 0 }, out[32];
	CHECK(ring_init(&r, 16, 2) == 0, "init");
	ring_write(&r, in, 10);
	ring_skip_to(&r, 4);
	CHECK(ring_fill(&r) == 6, "skip drops frames up to the position");
	ring_skip_to(&r, 2);
	CHECK(ring_fill(&r) == 6, "skip never moves backwards");
	ring_skip_to(&r, 1000);
	CHECK(ring_fill(&r) == 0 && ring_tail(&r) == ring_head(&r), "skip is clamped to what was written");
	CHECK(ring_read(&r, out, 4) == 0, "nothing is readable after skipping to the head");
	ring_free(&r);
	return 0;
}

static int markers(void)
{
	struct mm_markq q;
	struct mm_mark m;
	markq_init(&q);
	CHECK(!markq_peek(&q, &m), "a new queue is empty");
	for (uint32_t i = 0; i < MM_MARKQ_CAP; i++) {
		struct mm_mark k = { .pos = i * 10u, .item = i, .kind = MM_MARK_START };
		CHECK(markq_push(&q, &k) == 0, "markers up to capacity are accepted");
	}
	struct mm_mark extra = { .pos = 9999 };
	CHECK(markq_push(&q, &extra) != 0, "a full queue refuses another marker");
	for (uint32_t i = 0; i < MM_MARKQ_CAP; i++) {
		CHECK(markq_peek(&q, &m) && m.item == i && m.pos == i * 10u, "markers come out in order");
		markq_pop(&q);
	}
	CHECK(!markq_peek(&q, &m), "the queue drains");
	markq_pop(&q);
	CHECK(markq_push(&q, &extra) == 0, "popping an empty queue is harmless and it accepts again");
	return 0;
}

/* Threaded: producer writes a counting sequence in odd chunk sizes, the
 * consumer checks it. Exercises the acquire/release pairing (run under
 * TSan) and partial reads/writes across the wrap. */
#define SEQ_FRAMES (1u << 20)
static struct mm_ring s_r;

static void *producer(void *arg)
{
	(void)arg;
	float buf[2 * 97];
	uint32_t next = 0;
	while (next < SEQ_FRAMES) {
		uint32_t n = 1 + next % 97;
		if (n > SEQ_FRAMES - next) n = SEQ_FRAMES - next;
		for (uint32_t i = 0; i < n; i++) buf[2 * i] = buf[2 * i + 1] = (float)(next + i);
		uint32_t off = 0;
		while (off < n) off += ring_write(&s_r, buf + 2 * off, n - off);
		next += n;
	}
	return NULL;
}

static int threaded(void)
{
	CHECK(ring_init(&s_r, 1024, 2) == 0, "init");
	pthread_t t;
	CHECK(pthread_create(&t, NULL, producer, NULL) == 0, "producer starts");
	float buf[2 * 61];
	uint32_t expect = 0;
	int bad = 0;
	while (expect < SEQ_FRAMES) {
		uint32_t n = ring_read(&s_r, buf, 1 + expect % 61);
		for (uint32_t i = 0; i < n; i++)
			if (buf[2 * i] != (float)(expect + i) || buf[2 * i + 1] != (float)(expect + i)) bad = 1;
		expect += n;
	}
	pthread_join(t, NULL);
	CHECK(!bad, "a million frames cross threads intact and in order");
	ring_free(&s_r);
	return 0;
}

int main(void)
{
	RUN(basics);
	RUN(wrap64);
	RUN(skip);
	RUN(markers);
	RUN(threaded);
	CHECK_PASS("test-ring");
}

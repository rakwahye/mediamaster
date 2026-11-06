/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file watch.c
 * @brief Event lines to kept-open connections. */

#include "watch.h"
#include "ipc.h"
#include "log.h"
#include "mm_plugin.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "watch"

static const struct { const char *name; uint32_t bit; } NAMES[] = {
	{ "state", MM_EV_STATE }, { "track", MM_EV_TRACK }, { "volume", MM_EV_VOLUME },
	{ "seek", MM_EV_SEEK }, { "queue", MM_EV_QUEUE }, { "meta", MM_EV_META },
};
#define NNAMES (sizeof(NAMES) / sizeof(NAMES[0]))
#define ALL (MM_EV_STATE | MM_EV_TRACK | MM_EV_VOLUME | MM_EV_SEEK | MM_EV_QUEUE | MM_EV_META)

struct watcher { int fd; uint32_t mask; };

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static struct watcher s_w[WATCH_MAX];
static int s_n;

/* Names of the bits in `mask`, space-separated. */
static void names(uint32_t mask, char *out, size_t cap)
{
	size_t off = 0;
	out[0] = '\0';
	for (size_t i = 0; i < NNAMES && off < cap; i++)
		if (mask & NAMES[i].bit)
			off += (size_t)snprintf(out + off, cap - off, "%s%s", off ? " " : "", NAMES[i].name);
}

/* @returns 0 if all of s went out at once. A watcher gets whole lines or
 * nothing: one that cannot take a line now is dropped. */
static int put(int fd, const char *s)
{
	size_t n = strlen(s);
	ssize_t w;
	do w = send(fd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT);
	while (w < 0 && errno == EINTR);
	return w == (ssize_t)n ? 0 : -1;
}

int watch_adopt(int fd, int argc, char *const *argv)
{
	uint32_t mask = 0;
	char line[160];
	for (int i = 1; i < argc; i++) {
		size_t k = 0;
		while (k < NNAMES && strcmp(argv[i], NAMES[k].name)) k++;
		if (k == NNAMES) {
			snprintf(line, sizeof(line), "err unknown event: %.64s\n", argv[i]);
			put(fd, line);
			return 0;
		}
		mask |= NAMES[k].bit;
	}
	if (!mask) mask = ALL;

	pthread_mutex_lock(&s_lock);
	int full = s_n >= WATCH_MAX, kept = !full;
	if (kept) {
		s_w[s_n].fd = fd;
		s_w[s_n].mask = mask;
		s_n++;
		/* Under the lock, so no event line can go out before the ok. */
		char nm[96];
		names(mask, nm, sizeof(nm));
		snprintf(line, sizeof(line), "ok watching %s\n", nm);
		if (put(fd, line) != 0) {
			s_n--;
			kept = 0;
		}
	}
	pthread_mutex_unlock(&s_lock);
	if (full) put(fd, "err too many watchers\n");
	return kept;
}

/* A client that sent something after its frame is ignored; one that
 * closed its end is gone. */
static int hung_up(int fd)
{
	char b[256];
	for (;;) {
		ssize_t r = recv(fd, b, sizeof(b), MSG_DONTWAIT);
		if (r > 0) continue;
		if (r < 0 && errno == EINTR) continue;
		return r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
	}
}

void watch_tick(uint32_t ev)
{
	pthread_mutex_lock(&s_lock);
	int w = 0;
	for (int i = 0; i < s_n; i++) {
		int fd = s_w[i].fd, drop = hung_up(fd);
		uint32_t m = ev & s_w[i].mask;
		if (!drop && m) {
			char line[96];
			names(m, line, sizeof(line) - 1);
			strcat(line, "\n");
			drop = put(fd, line) != 0;
		}
		if (drop) {
			LOGD(TAG, "watcher %d gone", fd);
			close(fd);
			continue;
		}
		s_w[w++] = s_w[i];
	}
	s_n = w;
	pthread_mutex_unlock(&s_lock);
}

static int v_watch(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	snprintf(c->reply, c->reply_len, "watch needs a connection of its own (mmctl watch)");
	return MM_ERR;
}

int watch_init(void)
{
	ipc_set_adopt("watch", watch_adopt);
	return ipc_register("host", "watch", "[state|track|volume|seek|queue|meta]...\tkeep the connection: a line naming what changed, each time something does",
	                    v_watch, NULL);
}

void watch_fini(void)
{
	ipc_set_adopt(NULL, NULL);
	pthread_mutex_lock(&s_lock);
	for (int i = 0; i < s_n; i++) close(s_w[i].fd);
	s_n = 0;
	pthread_mutex_unlock(&s_lock);
}

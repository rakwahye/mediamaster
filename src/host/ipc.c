/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file ipc.c
 * @brief Unix-socket listener, frame decoder, verb dispatch. */

#include "ipc.h"
#include "log.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define TAG "ipc"
#define IPC_MAX_VERBS 128
#define IPC_IO_TIMEOUT_S 2

struct verb {
	char *owner, *verb, *help;
	mm_verb_fn fn;
	void *priv;
};

static pthread_mutex_t s_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct verb s_verbs[IPC_MAX_VERBS];
static int s_nverbs;

static int s_listen = -1;
static int s_wake = -1;
static pthread_t s_thread;
static atomic_int s_running;
static char s_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

/* ------------------------------------------------------------------ registry */

static int verb_name_ok(const char *v)
{
	if (!v || !*v) return 0;
	for (const char *c = v; *c; c++)
		if (*c <= ' ' || *c == 0x7f) return 0;
	return 1;
}

int ipc_register(const char *owner, const char *verb, const char *help,
                 mm_verb_fn fn, void *priv)
{
	if (!owner || !fn || !verb_name_ok(verb) || !strcmp(verb, "help")) return -1;
	int rc = -1;
	pthread_mutex_lock(&s_reg_lock);
	for (int i = 0; i < s_nverbs; i++) {
		if (!strcmp(s_verbs[i].verb, verb)) {
			LOGW(TAG, "%s: verb '%s' already owned by %s", owner, verb, s_verbs[i].owner);
			goto out;
		}
	}
	if (s_nverbs == IPC_MAX_VERBS) {
		LOGW(TAG, "%s: verb table full", owner);
		goto out;
	}
	struct verb *e = &s_verbs[s_nverbs];
	e->owner = strdup(owner);
	e->verb = strdup(verb);
	e->help = strdup(help ? help : "");
	if (!e->owner || !e->verb || !e->help) {
		free(e->owner); free(e->verb); free(e->help);
		goto out;
	}
	e->fn = fn;
	e->priv = priv;
	s_nverbs++;
	rc = 0;
out:
	pthread_mutex_unlock(&s_reg_lock);
	return rc;
}

void ipc_unregister_owner(const char *owner)
{
	pthread_mutex_lock(&s_reg_lock);
	int w = 0;
	for (int i = 0; i < s_nverbs; i++) {
		if (!strcmp(s_verbs[i].owner, owner)) {
			free(s_verbs[i].owner); free(s_verbs[i].verb); free(s_verbs[i].help);
			continue;
		}
		s_verbs[w++] = s_verbs[i];
	}
	s_nverbs = w;
	pthread_mutex_unlock(&s_reg_lock);
}

int ipc_has_verb(const char *verb)
{
	int found = 0;
	pthread_mutex_lock(&s_reg_lock);
	for (int i = 0; i < s_nverbs && !found; i++)
		found = !strcmp(s_verbs[i].verb, verb);
	pthread_mutex_unlock(&s_reg_lock);
	return found;
}

/* ------------------------------------------------------------------ framing */

struct frame {
	int argc;
	char *argv[IPC_MAX_ARGS + 1];
	const char *body;
	size_t body_len;
};

static int recv_more(int fd, char *buf, size_t cap, size_t *n)
{
	for (;;) {
		ssize_t r = recv(fd, buf + *n, cap - *n, 0);
		if (r > 0) { *n += (size_t)r; return 0; }
		if (r < 0 && errno == EINTR) continue;
		return -1; /* EOF, timeout, error */
	}
}

/* Decode one frame into buf (cap bytes). @returns 0, or -1 if malformed,
 * oversized, or the peer went away. */
static int frame_read(int fd, char *buf, size_t cap, struct frame *f)
{
	size_t n = 0;
	char *nl;
	while (!(nl = memchr(buf, '\n', n))) {
		if (n == cap) return -1;
		if (recv_more(fd, buf, cap, &n)) return -1;
	}
	*nl = '\0';
	const size_t off = (size_t)(nl - buf) + 1;
	char *hdr = buf;
	size_t hl = strlen(hdr);
	while (hl > 0 && hdr[hl - 1] == '\r') hdr[--hl] = '\0';

	long want = -1;
	char *a = strstr(hdr, " argc=");
	if (a) {
		char *end;
		want = strtol(a + 6, &end, 10);
		if (*end || want < 1 || want > IPC_MAX_ARGS) return -1;
		*a = '\0';
	}

	f->argc = 0;
	if (want > 0) {
		size_t scanned = off;
		long nuls = 0;
		for (;;) {
			for (; scanned < n && nuls < want; scanned++)
				if (buf[scanned] == '\0') nuls++;
			if (nuls == want) break;
			if (n == cap) return -1;
			if (recv_more(fd, buf, cap, &n)) return -1;
		}
		char *p = buf + off;
		for (long i = 0; i < want; i++) {
			f->argv[f->argc++] = p;
			p += strlen(p) + 1;
		}
		if (strcmp(f->argv[0], hdr) != 0) return -1; /* header and arg0 disagree */
		f->body = p;
		f->body_len = (size_t)(buf + n - p);
	} else {
		char *cur = hdr;
		while (*cur) {
			while (*cur == ' ' || *cur == '\t') cur++;
			if (!*cur) break;
			if (f->argc == IPC_MAX_ARGS) return -1;
			f->argv[f->argc++] = cur;
			while (*cur && *cur != ' ' && *cur != '\t') cur++;
			if (*cur) *cur++ = '\0';
		}
		f->body = buf + off;
		f->body_len = n - off;
	}
	f->argv[f->argc] = NULL;
	return f->argc > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ dispatch */

static void send_all(int fd, const char *s, size_t len)
{
	while (len > 0) {
		ssize_t w = send(fd, s, len, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) return;
		s += w;
		len -= (size_t)w;
	}
}

static void send_str(int fd, const char *s) { send_all(fd, s, strlen(s)); }

static void build_help(char *out, size_t cap)
{
	size_t off = 0;
	off += (size_t)snprintf(out, cap, "commands:\n  %-28s %s\n", "help", "this list");
	pthread_mutex_lock(&s_reg_lock);
	for (int i = 0; i < s_nverbs && off < cap; i++) {
		const char *h = s_verbs[i].help;
		const char *tab = strchr(h, '\t');
		char usage[128];
		if (tab) snprintf(usage, sizeof(usage), "%s %.*s", s_verbs[i].verb, (int)(tab - h), h);
		else snprintf(usage, sizeof(usage), "%s", s_verbs[i].verb);
		off += (size_t)snprintf(out + off, cap - off, "  %-28s %s\n", usage, tab ? tab + 1 : h);
	}
	pthread_mutex_unlock(&s_reg_lock);
}

static void handle_client(int fd)
{
	static char buf[IPC_FRAME_MAX]; /* IPC thread only */
	static char reply[IPC_REPLY_MAX];
	static char out[IPC_REPLY_MAX + 8];
	struct frame f;

	if (frame_read(fd, buf, sizeof(buf), &f) != 0) {
		send_str(fd, "err bad frame\n");
		return;
	}

	if (!strcmp(f.argv[0], "help")) {
		build_help(out, sizeof(out));
		send_str(fd, out);
		return;
	}

	struct verb v = { 0 };
	pthread_mutex_lock(&s_reg_lock);
	for (int i = 0; i < s_nverbs; i++)
		if (!strcmp(s_verbs[i].verb, f.argv[0])) { v = s_verbs[i]; break; }
	pthread_mutex_unlock(&s_reg_lock);

	if (!v.fn) {
		snprintf(out, sizeof(out), "err unknown verb: %s\n", f.argv[0]);
		send_str(fd, out);
		return;
	}

	reply[0] = '\0';
	struct mm_verb_ctx c = {
		.argc = f.argc, .argv = f.argv,
		.body = f.body, .body_len = f.body_len,
		.reply = reply, .reply_len = sizeof(reply),
	};
	int rc = v.fn(v.priv, &c);
	reply[sizeof(reply) - 1] = '\0';

	if (rc == MM_REPLY_RAW) {
		send_str(fd, reply);
		return;
	}
	size_t rl = strlen(reply);
	while (rl > 0 && reply[rl - 1] == '\n') reply[--rl] = '\0';
	snprintf(out, sizeof(out), "%s%s%s\n", rc == MM_OK ? "ok" : "err",
	         rl ? " " : "", reply);
	send_str(fd, out);
}

static int peer_is_us(int fd)
{
	struct ucred cr;
	socklen_t len = sizeof(cr);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0) return 0;
	return cr.uid == getuid();
}

static void *ipc_thread(void *arg)
{
	(void)arg;
	while (atomic_load(&s_running)) {
		struct pollfd pf[2] = { { s_listen, POLLIN, 0 }, { s_wake, POLLIN, 0 } };
		if (poll(pf, 2, -1) < 0) {
			if (errno == EINTR) continue;
			LOGE(TAG, "poll: %s", strerror(errno));
			break;
		}
		if (pf[1].revents) break;
		if (!(pf[0].revents & POLLIN)) continue;

		int fd = accept4(s_listen, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0) continue;
		struct timeval tv = { IPC_IO_TIMEOUT_S, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (peer_is_us(fd)) handle_client(fd);
		else LOGW(TAG, "rejected connection from another uid");
		close(fd);
	}
	return NULL;
}

/* ------------------------------------------------------------------ lifecycle */

static int someone_listening(const struct sockaddr_un *addr)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return 0;
	int up = connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0;
	close(fd);
	return up;
}

int ipc_start(const char *path)
{
	if (atomic_load(&s_running)) return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	size_t pl = strlen(path);
	if (pl == 0 || pl >= sizeof(addr.sun_path)) {
		LOGE(TAG, "socket path empty or too long: %s", path);
		return -1;
	}
	memcpy(addr.sun_path, path, pl + 1);

	if (someone_listening(&addr)) {
		LOGE(TAG, "another instance is answering on %s", path);
		return -1;
	}
	unlink(path); /* stale socket from a crash */

	s_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s_listen < 0) {
		LOGE(TAG, "socket: %s", strerror(errno));
		return -1;
	}
	if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		LOGE(TAG, "bind %s: %s", path, strerror(errno));
		goto fail;
	}
	if (chmod(path, 0600) != 0) {
		LOGE(TAG, "chmod %s: %s", path, strerror(errno));
		goto fail_unlink;
	}
	if (listen(s_listen, 8) != 0) {
		LOGE(TAG, "listen: %s", strerror(errno));
		goto fail_unlink;
	}
	s_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (s_wake < 0) {
		LOGE(TAG, "eventfd: %s", strerror(errno));
		goto fail_unlink;
	}
	memcpy(s_path, path, pl + 1);
	atomic_store(&s_running, 1);
	int err = pthread_create(&s_thread, NULL, ipc_thread, NULL);
	if (err) {
		LOGE(TAG, "pthread_create: %s", strerror(err));
		atomic_store(&s_running, 0);
		close(s_wake);
		s_wake = -1;
		goto fail_unlink;
	}
	LOGI(TAG, "listening on %s", path);
	return 0;

fail_unlink:
	unlink(path);
fail:
	close(s_listen);
	s_listen = -1;
	return -1;
}

void ipc_stop(void)
{
	if (!atomic_load(&s_running)) return;
	atomic_store(&s_running, 0);
	uint64_t one = 1;
	if (write(s_wake, &one, sizeof(one)) < 0) LOGW(TAG, "wake: %s", strerror(errno));
	pthread_join(s_thread, NULL);
	close(s_listen);
	close(s_wake);
	s_listen = s_wake = -1;
	unlink(s_path);
	s_path[0] = '\0';
}

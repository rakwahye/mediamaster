/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file mmctl.c
 * @brief Control client: one command per invocation.
 *
 *   mmctl <verb> [args...]   send, print reply; exit 1 on "err"
 *   mmctl launch [opts...]   start the daemon detached (opts go to it)
 *   mmctl                    daemon's live help, or offline usage
 *
 * Socket: $MM_SOCK, else the daemon's default. */

#include "paths.h"
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int connect_daemon(void)
{
	const char *path = mm_paths_socket();
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(a.sun_path)) return -1;
	memcpy(a.sun_path, path, strlen(path) + 1);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) return -1;
		p += w;
		len -= (size_t)w;
	}
	return 0;
}

/* @returns process exit code. */
static int command(int argc, char **argv)
{
	int fd = connect_daemon();
	if (fd < 0) {
		fprintf(stderr, "mmctl: daemon not running (%s)\n", mm_paths_socket());
		return 1;
	}
	char hdr[256];
	int hl = snprintf(hdr, sizeof(hdr), "%s argc=%d\n", argv[0], argc);
	if (hl < 0 || hl >= (int)sizeof(hdr)) {
		fprintf(stderr, "mmctl: verb too long\n");
		close(fd);
		return 1;
	}
	int bad = send_all(fd, hdr, (size_t)hl);
	for (int i = 0; i < argc && !bad; i++) bad = send_all(fd, argv[i], strlen(argv[i]) + 1);
	if (bad) {
		fprintf(stderr, "mmctl: send: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	char buf[8192];
	int first = 1, is_err = 0;
	for (;;) {
		ssize_t r = recv(fd, buf, sizeof(buf), 0);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		if (first) { is_err = r >= 3 && !memcmp(buf, "err", 3); first = 0; }
		fwrite(buf, 1, (size_t)r, is_err ? stderr : stdout);
	}
	close(fd);
	if (first) {
		fprintf(stderr, "mmctl: no reply\n");
		return 1;
	}
	return is_err ? 1 : 0;
}

/* Prefer the daemon next to this binary; fall back to PATH. */
static const char *daemon_path(char *buf, size_t cap)
{
	ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
	if (n > 0) {
		buf[n] = '\0';
		char *slash = strrchr(buf, '/');
		if (slash && (size_t)(slash - buf) + sizeof("/mediamaster") <= cap) {
			strcpy(slash, "/mediamaster");
			if (access(buf, X_OK) == 0) return buf;
		}
	}
	return "mediamaster";
}

static int launch(int argc, char **argv)
{
	int fd = connect_daemon();
	if (fd >= 0) {
		close(fd);
		fprintf(stderr, "mmctl: daemon already running\n");
		return 0;
	}
	char pbuf[PATH_MAX];
	const char *exe = daemon_path(pbuf, sizeof(pbuf));

	char *args[64];
	int n = 0;
	args[n++] = (char *)"mediamaster";
	args[n++] = (char *)"-D";
	for (int i = 1; i < argc && n < 63; i++) args[n++] = argv[i];
	args[n] = NULL;

	pid_t pid;
	int err = posix_spawnp(&pid, exe, NULL, NULL, args, environ);
	if (err) {
		fprintf(stderr, "mmctl: spawn %s: %s\n", exe, strerror(err));
		return 1;
	}
	int st;
	waitpid(pid, &st, 0); /* -D: first fork exits at once */

	for (int i = 0; i < 50; i++) {
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, NULL);
		fd = connect_daemon();
		if (fd >= 0) {
			close(fd);
			fprintf(stderr, "mmctl: daemon started\n");
			return 0;
		}
	}
	fprintf(stderr, "mmctl: daemon did not answer within 5s; see %s\n", mm_paths_log_file());
	return 1;
}

static void usage(void)
{
	fprintf(stderr,
	        "usage: mmctl <command> [args...]\n"
	        "       mmctl launch [-c config] [-o key=value]... [-d]\n"
	        "\n"
	        "The command list comes from the running daemon: start it, then 'mmctl help'.\n");
}

int main(int argc, char **argv)
{
	static char *help_argv[] = { (char *)"help", NULL };
	if (argc < 2) {
		int fd = connect_daemon();
		if (fd < 0) { usage(); return 1; }
		close(fd);
		return command(1, help_argv);
	}
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		int fd = connect_daemon();
		if (fd < 0) { usage(); return 0; }
		close(fd);
		return command(1, help_argv);
	}
	if (!strcmp(argv[1], "launch")) return launch(argc - 1, argv + 1);
	return command(argc - 1, argv + 1);
}

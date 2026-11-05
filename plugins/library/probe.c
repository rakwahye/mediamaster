/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file probe.c
 * @brief ffprobe as a child: argv only, an empty signal mask, default
 * dispositions, /dev/null for stdin and stderr, every fd of ours
 * close-on-exec. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* versionsort, strverscmp, pipe2, d_type */
#endif
#include "probe.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define OUT_MAX 16384

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ffprobe's tag name → ours. */
static const char *const tag_map[][2] = {
	{ "title", "title" }, { "artist", "artist" }, { "album", "album" },
	{ "album_artist", "albumartist" }, { "albumartist", "albumartist" },
	{ "track", "tracknumber" }, { "tracknumber", "tracknumber" },
	{ "disc", "discnumber" }, { "discnumber", "discnumber" },
	{ "date", "date" }, { "year", "date" }, { "genre", "genre" },
};

static int same_ci(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a) != ((*b >= 'A' && *b <= 'Z') ? *b + 32 : *b)) return 0;
	return *a == *b;
}

/* "TAG:name=value" lines → tags; the first of each key wins (the audio
 * stream's come before the container's), except artist, which ffprobe
 * gives once with separators the tagger chose: kept as one value. */
static void parse(char *buf, char *tags, size_t cap)
{
	size_t o = 0;
	char seen[16][16];
	int nseen = 0;
	tags[0] = '\0';
	for (char *line = buf, *next; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next) *next++ = '\0';
		if (strncmp(line, "TAG:", 4) != 0) continue;
		char *eq = strchr(line + 4, '=');
		if (!eq || !eq[1]) continue;
		*eq = '\0';
		const char *key = NULL;
		for (size_t i = 0; i < sizeof(tag_map) / sizeof(tag_map[0]) && !key; i++)
			if (same_ci(line + 4, tag_map[i][0])) key = tag_map[i][1];
		int dup = 0;
		for (int i = 0; key && i < nseen && !dup; i++) dup = !strcmp(seen[i], key);
		if (!key || dup || nseen == 16) continue;
		snprintf(seen[nseen++], sizeof(seen[0]), "%s", key);
		char *v = eq + 1;
		if (!strcmp(key, "tracknumber") || !strcmp(key, "discnumber")) {
			char *slash = strchr(v, '/');
			if (slash) *slash = '\0';
		}
		for (char *c = v; *c; c++)
			if (*c == '\t' || *c == '\r') *c = ' ';
		int w = snprintf(tags + o, cap - o, "%s=%s\n", key, v);
		if (w < 0 || (size_t)w >= cap - o) {
			tags[o] = '\0';
			break;
		}
		o += (size_t)w;
	}
}

long long probe_file(const char *ffprobe, const char *path, long timeout_ms, char *tags, size_t cap)
{
	char input[4096 + 8];
	char *argv[] = {
		(char *)ffprobe, (char *)"-v", (char *)"error",
		(char *)"-select_streams", (char *)"a:0",
		(char *)"-show_entries", (char *)"stream_tags:format=duration:format_tags",
		(char *)"-of", (char *)"default=noprint_wrappers=1", input, NULL
	};
	int po[2];
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	sigset_t none, all;
	pid_t pid;

	tags[0] = '\0';
	snprintf(input, sizeof(input), "file:%s", path); /* never an option or a protocol */
	if (pipe2(po, O_CLOEXEC) != 0) return -1;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawnattr_init(&at);
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setsigdefault(&at, &all);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	int rc = posix_spawnp(&pid, ffprobe, &fa, &at, argv, environ);
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	close(po[1]);
	if (rc != 0) {
		close(po[0]);
		return -1;
	}

	char *out = malloc(OUT_MAX);
	size_t len = 0;
	long deadline = now_ms() + timeout_ms;
	int timed_out = 0;
	while (out) {
		long left = deadline - now_ms();
		if (left <= 0) {
			timed_out = 1;
			break;
		}
		struct pollfd pf = { po[0], POLLIN, 0 };
		int pr = poll(&pf, 1, (int)left);
		if (pr < 0 && errno == EINTR) continue;
		if (pr <= 0) continue;
		char sink[512];
		int full = len == OUT_MAX - 1;
		ssize_t r = full ? read(po[0], sink, sizeof(sink)) : read(po[0], out + len, OUT_MAX - 1 - len);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		if (!full) len += (size_t)r;
	}
	close(po[0]);
	if (timed_out) kill(pid, SIGKILL);
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
	if (!out) return 0;
	out[len] = '\0';

	long long ms = 0;
	char *d = strstr(out, "duration=");
	if (d && (d == out || d[-1] == '\n')) {
		char *end;
		double secs = strtod(d + 9, &end);
		if (end != d + 9 && secs > 0) ms = (long long)(secs * 1000.0 + 0.5);
	}
	if (!timed_out) parse(out, tags, cap);
	free(out);
	return ms;
}

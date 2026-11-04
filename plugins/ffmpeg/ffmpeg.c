/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file ffmpeg.c
 * @brief Decode anything ffmpeg can read, by running it.
 *
 * One ffmpeg child per stream, decoding to f32le 48 kHz stereo on a pipe:
 *
 *   ffmpeg -nostdin -hide_banner -loglevel error [-ss S] -i INPUT
 *          -map 0:a:0 -vn -sn -dn -f f32le -ar 48000 -ac 2 pipe:1
 *
 * Duration comes from ffprobe at open. Known duration → seekable (seek
 * respawns with -ss); unknown → live (radio, endless streams).
 *
 * URIs: file:/path, file:///path, a bare path, http://, https://.
 * Local paths are passed as "file:<path>" so a name starting with '-' or
 * containing ':' is never read as an option or a protocol.
 *
 * Children get an empty signal mask and default dispositions (the daemon
 * blocks its signals for signalfd), /dev/null on stdin, and nothing else
 * of ours: every pipe is close-on-exec.
 *
 * Config:
 *   ffmpeg.bin          = ffmpeg
 *   ffmpeg.ffprobe      = ffprobe
 *   ffmpeg.probe_ms     = 5000   give up on ffprobe after this (→ live)
 *   ffmpeg.net_timeout  = 15     seconds; ffmpeg -rw_timeout for http(s) */

#include "mm_plugin.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define FRAME_BYTES (MM_PCM_CHANNELS * sizeof(float))
#define ERRTAIL 512
#define KILL_GRACE_MS 1000

struct ff_priv {
	const struct mm_host *h;
};

struct ff_stream {
	const struct mm_host *h;
	char input[2048];      /* what ffmpeg gets after -i */
	char uri[2048];        /* for log lines */
	int is_net;
	pid_t pid;
	int out;               /* child's stdout */
	int err;               /* child's stderr, nonblocking */
	int wake[2];           /* self-pipe: interrupt */
	unsigned char part[FRAME_BYTES]; /* partial frame carried between reads */
	size_t part_len;
	char errtail[ERRTAIL]; /* last bytes ffmpeg said on stderr */
	size_t errlen;
};

/* ------------------------------------------------------------------ helpers */

static const char *cfg(const struct mm_host *h, const char *k, const char *def)
{
	const char *v = h->config_get(h, k);
	return (v && *v) ? v : def;
}

static long cfg_long(const struct mm_host *h, const char *k, long def)
{
	const char *v = h->config_get(h, k);
	return (v && *v) ? atol(v) : def;
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Spawn argv with stdout → *out and stderr → *err (each may be NULL for
 * /dev/null). @returns pid or -1. */
static pid_t spawn(const struct mm_host *h, char *const argv[], int *out, int *err)
{
	int po[2] = { -1, -1 }, pe[2] = { -1, -1 };
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	pid_t pid = -1;

	if (out && pipe2(po, O_CLOEXEC) != 0) return -1;
	if (err && pipe2(pe, O_CLOEXEC | O_NONBLOCK) != 0) goto close_pipes;
	if (posix_spawn_file_actions_init(&fa) != 0) goto close_pipes;
	if (posix_spawnattr_init(&at) != 0) {
		posix_spawn_file_actions_destroy(&fa);
		goto close_pipes;
	}

	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	if (out) posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	if (err) posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
	else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

	sigset_t none, all;
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setsigdefault(&at, &all);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

	int rc = posix_spawnp(&pid, argv[0], &fa, &at, argv, environ);
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) {
		h->log(h, MM_LOG_WARN, "cannot run %s: %s", argv[0], strerror(rc));
		pid = -1;
		goto close_pipes;
	}
	if (out) { close(po[1]); *out = po[0]; }
	if (err) { close(pe[1]); *err = pe[0]; }
	return pid;

close_pipes:
	if (po[0] >= 0) { close(po[0]); close(po[1]); }
	if (pe[0] >= 0) { close(pe[0]); close(pe[1]); }
	return -1;
}

/* SIGTERM, wait up to the grace period, then SIGKILL. Always reaps. */
static int reap(pid_t pid)
{
	int st = 0;
	if (pid <= 0) return 0;
	pid_t r = waitpid(pid, &st, WNOHANG);
	if (r == pid) return st;
	kill(pid, SIGTERM);
	long deadline = now_ms() + KILL_GRACE_MS;
	while ((r = waitpid(pid, &st, WNOHANG)) == 0 && now_ms() < deadline) {
		struct timespec ts = { 0, 5 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	if (r == 0) {
		kill(pid, SIGKILL);
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
	}
	return st;
}

/* ------------------------------------------------------------------ probe */

/* Duration in ms via ffprobe, 0 if unknown, unknowable or too slow. */
static uint64_t probe_duration(struct ff_stream *s)
{
	const struct mm_host *h = s->h;
	char tmo[32];
	snprintf(tmo, sizeof(tmo), "%ld", cfg_long(h, "net_timeout", 15) * 1000000L);
	char *argv[16];
	int n = 0;
	argv[n++] = (char *)cfg(h, "ffprobe", "ffprobe");
	argv[n++] = (char *)"-v";
	argv[n++] = (char *)"error";
	if (s->is_net) { argv[n++] = (char *)"-rw_timeout"; argv[n++] = tmo; }
	argv[n++] = (char *)"-show_entries";
	argv[n++] = (char *)"format=duration";
	argv[n++] = (char *)"-of";
	argv[n++] = (char *)"default=noprint_wrappers=1:nokey=1";
	argv[n++] = s->input;
	argv[n] = NULL;

	int out = -1;
	pid_t pid = spawn(h, argv, &out, NULL);
	if (pid < 0) return 0;

	char buf[128];
	size_t len = 0;
	long deadline = now_ms() + cfg_long(h, "probe_ms", 5000);
	for (;;) {
		long left = deadline - now_ms();
		if (left <= 0) {
			h->log(h, MM_LOG_INFO, "ffprobe timed out on %s; treating as live", s->uri);
			break;
		}
		struct pollfd pf = { out, POLLIN, 0 };
		int pr = poll(&pf, 1, (int)left);
		if (pr < 0 && errno == EINTR) continue;
		if (pr <= 0) continue;
		ssize_t r = read(out, buf + len, sizeof(buf) - 1 - len);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		len += (size_t)r;
		if (len == sizeof(buf) - 1) break;
	}
	close(out);
	reap(pid);
	buf[len] = '\0';

	char *end;
	double secs = strtod(buf, &end);
	if (end == buf || secs <= 0) return 0; /* "N/A" or nothing */
	return (uint64_t)(secs * 1000.0 + 0.5);
}

/* ------------------------------------------------------------------ decode */

static void drain_stderr(struct ff_stream *s)
{
	if (s->err < 0) return;
	char buf[ERRTAIL];
	for (;;) {
		ssize_t r = read(s->err, buf, sizeof(buf));
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return;
		/* keep the tail */
		size_t n = (size_t)r;
		if (n >= ERRTAIL - 1) {
			memcpy(s->errtail, buf + n - (ERRTAIL - 1), ERRTAIL - 1);
			s->errlen = ERRTAIL - 1;
		} else {
			if (s->errlen + n > ERRTAIL - 1) {
				size_t drop = s->errlen + n - (ERRTAIL - 1);
				memmove(s->errtail, s->errtail + drop, s->errlen - drop);
				s->errlen -= drop;
			}
			memcpy(s->errtail + s->errlen, buf, n);
			s->errlen += n;
		}
	}
}

static int decoder_start(struct ff_stream *s, uint64_t from_ms)
{
	const struct mm_host *h = s->h;
	char ss[32], tmo[32];
	char *argv[32];
	int n = 0;
	argv[n++] = (char *)cfg(h, "bin", "ffmpeg");
	argv[n++] = (char *)"-nostdin";
	argv[n++] = (char *)"-hide_banner";
	argv[n++] = (char *)"-loglevel";
	argv[n++] = (char *)"error";
	if (s->is_net) {
		snprintf(tmo, sizeof(tmo), "%ld", cfg_long(h, "net_timeout", 15) * 1000000L);
		argv[n++] = (char *)"-rw_timeout";
		argv[n++] = tmo;
		argv[n++] = (char *)"-reconnect";
		argv[n++] = (char *)"1";
	}
	if (from_ms) {
		snprintf(ss, sizeof(ss), "%llu.%03u", (unsigned long long)(from_ms / 1000), (unsigned)(from_ms % 1000));
		argv[n++] = (char *)"-ss";
		argv[n++] = ss;
	}
	argv[n++] = (char *)"-i";
	argv[n++] = s->input;
	argv[n++] = (char *)"-map";
	argv[n++] = (char *)"0:a:0";
	argv[n++] = (char *)"-vn";
	argv[n++] = (char *)"-sn";
	argv[n++] = (char *)"-dn";
	argv[n++] = (char *)"-f";
	argv[n++] = (char *)"f32le";
	argv[n++] = (char *)"-ar";
	argv[n++] = (char *)"48000";
	argv[n++] = (char *)"-ac";
	argv[n++] = (char *)"2";
	argv[n++] = (char *)"pipe:1";
	argv[n] = NULL;

	s->out = s->err = -1;
	s->part_len = 0;
	s->errlen = 0;
	s->pid = spawn(h, argv, &s->out, &s->err);
	return s->pid < 0 ? -1 : 0;
}

/* Stop the child and report how it ended. `expected` = we asked it to stop. */
static void decoder_stop(struct ff_stream *s, int expected)
{
	if (s->pid <= 0) return;
	if (s->out >= 0) { close(s->out); s->out = -1; } /* EPIPE ends a writing ffmpeg too */
	int st = reap(s->pid);
	s->pid = -1;
	drain_stderr(s);
	if (s->err >= 0) { close(s->err); s->err = -1; }
	if (!expected && !(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
		while (s->errlen && (s->errtail[s->errlen - 1] == '\n' || s->errtail[s->errlen - 1] == '\r')) s->errlen--;
		s->errtail[s->errlen] = '\0';
		const char *msg = s->errtail;
		const char *nl = strrchr(msg, '\n');
		s->h->log(s->h, MM_LOG_WARN, "%s: ffmpeg %s %d%s%s", s->uri,
		          WIFEXITED(st) ? "exited" : "killed by signal",
		          WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st),
		          s->errlen ? ": " : "", nl ? nl + 1 : msg);
	}
}

/* ------------------------------------------------------------------ ops */

static int uri_to_input(const char *uri, char *out, size_t cap, int *is_net)
{
	*is_net = 0;
	if (!strncmp(uri, "http://", 7) || !strncmp(uri, "https://", 8)) {
		*is_net = 1;
		return snprintf(out, cap, "%s", uri) < (int)cap ? 0 : -1;
	}
	const char *path = uri;
	if (!strncmp(uri, "file://", 7)) path = uri + 7;
	else if (!strncmp(uri, "file:", 5)) path = uri + 5;
	if (path[0] != '/') return -1; /* the daemon has no meaningful cwd */
	return snprintf(out, cap, "file:%s", path) < (int)cap ? 0 : -1;
}

static int ff_open(void *priv, const char *uri, void **stream, struct mm_stream_info *info)
{
	struct ff_priv *p = priv;
	struct ff_stream *s = calloc(1, sizeof(*s));
	if (!s) return -1;
	s->h = p->h;
	s->pid = -1;
	s->out = s->err = -1;
	s->wake[0] = s->wake[1] = -1;
	snprintf(s->uri, sizeof(s->uri), "%s", uri);

	if (uri_to_input(uri, s->input, sizeof(s->input), &s->is_net) != 0) {
		p->h->log(p->h, MM_LOG_WARN, "not a usable path or URL: %s", uri);
		goto fail;
	}
	if (!s->is_net) {
		struct stat st;
		errno = 0;
		if (stat(s->input + 5, &st) != 0 || !S_ISREG(st.st_mode)) {
			p->h->log(p->h, MM_LOG_WARN, "%s: %s", s->input + 5,
			          errno ? strerror(errno) : "not a regular file");
			goto fail;
		}
	}
	if (pipe2(s->wake, O_CLOEXEC | O_NONBLOCK) != 0) goto fail;

	info->duration_ms = probe_duration(s);
	info->flags = info->duration_ms ? MM_STREAM_SEEKABLE : MM_STREAM_LIVE;

	if (decoder_start(s, 0) != 0) goto fail;
	*stream = s;
	return 0;

fail:
	if (s->wake[0] >= 0) { close(s->wake[0]); close(s->wake[1]); }
	free(s);
	return -1;
}

static void clear_wake(struct ff_stream *s)
{
	char b[64];
	while (read(s->wake[0], b, sizeof(b)) > 0) { }
}

static long ff_read(void *stream, float *dst, uint32_t frames)
{
	struct ff_stream *s = stream;
	if (s->out < 0) return 0;
	unsigned char *d = (unsigned char *)dst;
	size_t want = (size_t)frames * FRAME_BYTES;
	size_t have = s->part_len;
	memcpy(d, s->part, have);

	while (have < FRAME_BYTES) {
		struct pollfd pf[3] = {
			{ s->out, POLLIN, 0 },
			{ s->wake[0], POLLIN, 0 },
			{ s->err, POLLIN, 0 },
		};
		int pr = poll(pf, s->err >= 0 ? 3 : 2, -1);
		if (pr < 0) {
			if (errno == EINTR) continue;
			return -errno;
		}
		if (pf[1].revents) return 0; /* interrupted; partial frame kept */
		if (s->err >= 0 && pf[2].revents) drain_stderr(s);
		if (!(pf[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;

		ssize_t r = read(s->out, d + have, want - have);
		if (r < 0) {
			if (errno == EINTR) continue;
			return -errno;
		}
		if (r == 0) {          /* end of stream, or ffmpeg died */
			decoder_stop(s, 0);
			s->part_len = 0;   /* a dangling partial frame is dropped */
			return 0;
		}
		have += (size_t)r;
	}
	size_t whole = have - have % FRAME_BYTES;
	s->part_len = have - whole;
	memcpy(s->part, d + whole, s->part_len);
	return (long)(whole / FRAME_BYTES);
}

static int ff_seek(void *stream, uint64_t ms)
{
	struct ff_stream *s = stream;
	decoder_stop(s, 1);
	clear_wake(s);
	return decoder_start(s, ms);
}

static void ff_interrupt(void *stream)
{
	struct ff_stream *s = stream;
	char c = 1;
	ssize_t w = write(s->wake[1], &c, 1);
	(void)w; /* full pipe = already pending */
}

static void ff_close(void *stream)
{
	struct ff_stream *s = stream;
	decoder_stop(s, 1);
	close(s->wake[0]);
	close(s->wake[1]);
	free(s);
}

static int ff_init(const struct mm_host *h, void **priv)
{
	struct ff_priv *p = calloc(1, sizeof(*p));
	if (!p) return -1;
	p->h = h;
	*priv = p;
	return 0;
}

static void ff_fini(void *priv)
{
	free(priv);
}

static const struct mm_source_ops s_source = {
	.open = ff_open,
	.read = ff_read,
	.seek = ff_seek,
	.interrupt = ff_interrupt,
	.close = ff_close,
};

static const struct mm_plugin s_plugin = {
	.abi = MM_ABI,
	.attrs = MM_ATTR_SPAWNS | MM_ATTR_NETWORK,
	.name = "ffmpeg",
	.version = "0.1.0",
	.description = "files and http(s) streams, decoded by ffmpeg",
	.schemes = "file,http,https",
	.init = ff_init,
	.fini = ff_fini,
	.source = &s_source,
};

MM_PLUGIN_DEFINE(ffmpeg)
{
	return &s_plugin;
}

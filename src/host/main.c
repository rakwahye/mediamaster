/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file main.c
 * @brief mediamaster daemon entry point.
 *
 * Startup order: config, (daemonize), signals, host verbs, plugins, IPC.
 * The main thread then sleeps in poll() on a signalfd and a wake eventfd,
 * and runs the work that must not happen on the IPC thread (reload,
 * shutdown). Every signal is blocked before any thread exists, so the
 * signalfd sees all of them. */

#include "builtins.h"
#include "config.h"
#include "ipc.h"
#include "loader.h"
#include "log.h"
#include "paths.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "main"

#ifndef MM_VERSION
#define MM_VERSION "0.0.0-dev"
#endif

static int s_wake = -1;
static atomic_int s_quit;
static atomic_int s_reload;

static void wake_main(void)
{
	uint64_t one = 1;
	if (write(s_wake, &one, sizeof(one)) < 0 && errno != EAGAIN)
		LOGW(TAG, "wake: %s", strerror(errno));
}

/* ------------------------------------------------------------------ host verbs */

static int verb_quit(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	atomic_store(&s_quit, 1);
	wake_main();
	snprintf(c->reply, c->reply_len, "bye");
	return MM_OK;
}

static int verb_reload(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	atomic_store(&s_reload, 1);
	wake_main();
	snprintf(c->reply, c->reply_len, "reload queued (%s)", config_path());
	return MM_OK;
}

static int verb_plugins(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	loader_describe(c->reply, c->reply_len);
	if (!c->reply[0]) snprintf(c->reply, c->reply_len, "no plugins loaded\n");
	return MM_REPLY_RAW;
}

static int verb_version(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	snprintf(c->reply, c->reply_len, "mediamaster %s abi %d", MM_VERSION, MM_ABI);
	return MM_OK;
}

static int register_host_verbs(void)
{
	int rc = 0;
	rc |= ipc_register("host", "quit", "\tshut the daemon down", verb_quit, NULL);
	rc |= ipc_register("host", "reload", "\tre-read the config file", verb_reload, NULL);
	rc |= ipc_register("host", "plugins", "\tloaded plugins, interfaces, state", verb_plugins, NULL);
	rc |= ipc_register("host", "version", "\tdaemon and ABI version", verb_version, NULL);
	return rc;
}

/* ------------------------------------------------------------------ process */

static void usage(FILE *f)
{
	fprintf(f,
	        "usage: mediamaster [-c config] [-o key=value]... [-d] [-D]\n"
	        "  -c PATH        config file (default %s)\n"
	        "  -o KEY=VALUE   override a config key; survives reload\n"
	        "  -d             debug logging\n"
	        "  -D             detach and log to %s\n"
	        "  -V             version\n",
	        mm_paths_config_file(), mm_paths_log_file());
}

static int daemonize(const char *log_path)
{
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid > 0) _exit(0);
	if (setsid() < 0) return -1;
	pid = fork();
	if (pid < 0) return -1;
	if (pid > 0) _exit(0);

	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", log_path);
	char *slash = strrchr(dir, '/');
	if (slash) { *slash = '\0'; mm_mkdir_p(dir, 0700); }

	int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
	int log = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (nul >= 0) { dup2(nul, STDIN_FILENO); dup2(nul, STDOUT_FILENO); }
	if (log >= 0) dup2(log, STDERR_FILENO);
	if (nul > STDERR_FILENO) close(nul);
	if (log > STDERR_FILENO) close(log);
	if (chdir("/") != 0) return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *cfg_path = NULL;
	int debug = 0, detach = 0, opt;

	while ((opt = getopt(argc, argv, "c:o:dDVh")) != -1) {
		switch (opt) {
		case 'c': cfg_path = optarg; break;
		case 'o':
			if (config_override(optarg) != 0) {
				fprintf(stderr, "mediamaster: bad -o '%s' (want key=value)\n", optarg);
				return 2;
			}
			break;
		case 'd': debug = 1; break;
		case 'D': detach = 1; break;
		case 'V': printf("mediamaster %s abi %d\n", MM_VERSION, MM_ABI); return 0;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}

	config_load(cfg_path ? cfg_path : mm_paths_config_file());
	const char *lvl = config_get_str("host.log_level", "info");
	mm_log_set_level(debug ? MM_LOG_DEBUG
	                 : !strcmp(lvl, "debug") ? MM_LOG_DEBUG
	                 : !strcmp(lvl, "warn") ? MM_LOG_WARN
	                 : !strcmp(lvl, "error") ? MM_LOG_ERROR : MM_LOG_INFO);

	if (detach && daemonize(mm_paths_log_file()) != 0) {
		LOGE(TAG, "daemonize: %s", strerror(errno));
		return 1;
	}
	LOGI(TAG, "mediamaster %s (pid %d)", MM_VERSION, (int)getpid());

	/* Block before any thread starts; threads inherit the mask. */
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);
	int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
	s_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (sfd < 0 || s_wake < 0) {
		LOGE(TAG, "signalfd/eventfd: %s", strerror(errno));
		return 1;
	}

	if (register_host_verbs() != 0) {
		LOGE(TAG, "host verb registration failed");
		return 1;
	}

	if (config_get_bool("host.builtins", 1)) builtins_add_all();
	loader_scan(config_get_str("host.plugin_dir", mm_paths_plugin_dirs()));
	loader_init_all();

	const char *sock = config_get_str("host.socket", mm_paths_socket());
	if (ipc_start(sock) != 0) {
		loader_fini_all();
		ipc_unregister_owner("host");
		config_free();
		return 1;
	}

	while (!atomic_load(&s_quit)) {
		struct pollfd pf[2] = { { sfd, POLLIN, 0 }, { s_wake, POLLIN, 0 } };
		if (poll(pf, 2, -1) < 0) {
			if (errno == EINTR) continue;
			LOGE(TAG, "poll: %s", strerror(errno));
			break;
		}
		if (pf[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
				if (si.ssi_signo == SIGHUP) atomic_store(&s_reload, 1);
				else if (si.ssi_signo != SIGPIPE) atomic_store(&s_quit, 1);
			}
		}
		if (pf[1].revents & POLLIN) {
			uint64_t v;
			if (read(s_wake, &v, sizeof(v)) < 0 && errno != EAGAIN)
				LOGW(TAG, "wake read: %s", strerror(errno));
		}
		if (atomic_exchange(&s_reload, 0)) {
			LOGI(TAG, "reloading %s", config_path());
			config_reload();
			loader_reload_all();
		}
	}

	LOGI(TAG, "shutting down");
	ipc_stop();
	loader_fini_all();
	ipc_unregister_owner("host");
	config_free();
	close(sfd);
	close(s_wake);
	return 0;
}

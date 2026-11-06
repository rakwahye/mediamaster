/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file paths.c
 * @brief XDG path resolution. */

#include "paths.h"
#include "mm_client.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MM_PLUGIN_DIR
#define MM_PLUGIN_DIR "/usr/local/lib/mediamaster/plugins"
#endif

#define PATH_CAP 1024

static const char *xdg(const char *var, const char *home_rel, char *buf)
{
	const char *v = getenv(var);
	if (v && v[0] == '/') {
		snprintf(buf, PATH_CAP, "%s/" MM_APP, v);
		return buf;
	}
	const char *home = getenv("HOME");
	snprintf(buf, PATH_CAP, "%s/%s/" MM_APP, (home && *home) ? home : "/tmp", home_rel);
	return buf;
}

const char *mm_paths_config_dir(void)
{
	static char buf[PATH_CAP];
	if (!buf[0]) xdg("XDG_CONFIG_HOME", ".config", buf);
	return buf;
}

const char *mm_paths_config_file(void)
{
	static char buf[PATH_CAP + 16];
	if (!buf[0]) snprintf(buf, sizeof(buf), "%s/config", mm_paths_config_dir());
	return buf;
}

const char *mm_paths_state_dir(void)
{
	static char buf[PATH_CAP];
	if (!buf[0]) xdg("XDG_STATE_HOME", ".local/state", buf);
	return buf;
}

const char *mm_paths_log_file(void)
{
	static char buf[PATH_CAP + 32];
	if (!buf[0]) snprintf(buf, sizeof(buf), "%s/" MM_APP ".log", mm_paths_state_dir());
	return buf;
}

/* The rule clients follow (mm_client.h) is the one the daemon follows. */
const char *mm_paths_socket(void)
{
	static char buf[PATH_CAP];
	if (!buf[0]) mm_client_socket(buf, sizeof(buf)); /* too long: truncated, fails at connect */
	return buf;
}

const char *mm_paths_plugin_dirs(void)
{
	static char buf[PATH_CAP * 2];
	if (!buf[0]) snprintf(buf, sizeof(buf), "%s/plugins:%s", mm_paths_config_dir(), MM_PLUGIN_DIR);
	return buf;
}

int mm_mkdir_p(const char *dir, unsigned mode)
{
	char tmp[PATH_CAP];
	if (snprintf(tmp, sizeof(tmp), "%s", dir) >= (int)sizeof(tmp)) return -1;
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) return -1;
		*p = '/';
	}
	if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) return -1;
	return 0;
}

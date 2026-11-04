/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file log.c
 * @brief stderr logging. One line per call, whole line under the stdio lock. */

#include "log.h"
#include <stdio.h>
#include <stdatomic.h>

static atomic_int s_level = MM_LOG_INFO;

void mm_log_set_level(int level) { atomic_store(&s_level, level); }
int mm_log_level(void) { return atomic_load(&s_level); }

void mm_vlog(int level, const char *tag, const char *fmt, va_list ap)
{
	if (level > atomic_load(&s_level)) return;
	static const char *const pfx[] = { "error: ", "warn: ", "", "" };
	flockfile(stderr);
	fprintf(stderr, "[%s] %s", tag ? tag : "?",
	        (level >= 0 && level <= MM_LOG_DEBUG) ? pfx[level] : "");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	funlockfile(stderr);
}

void mm_log(int level, const char *tag, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mm_vlog(level, tag, fmt, ap);
	va_end(ap);
}

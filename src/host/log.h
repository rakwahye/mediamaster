/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file log.h
 * @brief stderr logging, tagged by subsystem or plugin. */

#ifndef MM_LOG_H
#define MM_LOG_H

#include <stdarg.h>
#include "mm_plugin.h"

void mm_log_set_level(int level);
int mm_log_level(void);

void mm_vlog(int level, const char *tag, const char *fmt, va_list ap);

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void mm_log(int level, const char *tag, const char *fmt, ...);

#define LOGE(tag, ...) mm_log(MM_LOG_ERROR, tag, __VA_ARGS__)
#define LOGW(tag, ...) mm_log(MM_LOG_WARN, tag, __VA_ARGS__)
#define LOGI(tag, ...) mm_log(MM_LOG_INFO, tag, __VA_ARGS__)
#define LOGD(tag, ...) mm_log(MM_LOG_DEBUG, tag, __VA_ARGS__)

#endif

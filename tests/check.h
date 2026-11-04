/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file check.h
 * @brief Minimal test assertions. Each test binary returns nonzero on the
 * first failure, naming the expectation in plain words. */

#ifndef MM_CHECK_H
#define MM_CHECK_H

#include <stdio.h>

#define CHECK(cond, why) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d\n  want: %s\n  expr: %s\n", \
		        __FILE__, __LINE__, (why), #cond); \
		return 1; \
	} \
} while (0)

#define RUN(fn) do { if ((fn)() != 0) return 1; } while (0)

#define CHECK_PASS(name) do { \
	printf("%s: all checks passed\n", (name)); \
	return 0; \
} while (0)

#endif

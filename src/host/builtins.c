/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file builtins.c
 * @brief Plugins linked into the daemon. Same struct as a .so; loaded first,
 * so a shared object with the same name is skipped. Turn off with
 * `host.builtins = 0` to load everything from the plugin path. */

#include "builtins.h"
#include "loader.h"
#include <stddef.h>

const struct mm_plugin *mm_static_entry_null(void);

static mm_plugin_entry_fn const s_builtins[] = {
	mm_static_entry_null,
};

void builtins_add_all(void)
{
	for (size_t i = 0; i < sizeof(s_builtins) / sizeof(s_builtins[0]); i++)
		loader_add(s_builtins[i](), NULL, "builtin");
}

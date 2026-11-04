/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "loader.h"
#include "config.h"
#include "ipc.h"
#include "check.h"
#include <stdio.h>
#include <string.h>

static int g_init, g_fini, g_reload;

static int ok_verb(void *priv, struct mm_verb_ctx *c) { (void)priv; (void)c; return MM_OK; }

static int good_init(const struct mm_host *h, void **priv)
{
	g_init++;
	*priv = &g_init;
	if (h->abi != MM_ABI || h->size != sizeof(struct mm_host)) return -1;
	const char *v = h->config_get(h, "colour");
	if (!v || strcmp(v, "teal")) return -2; /* config slice is prefixed with the plugin name */
	return h->register_verb(h, "good", "\tgood verb", ok_verb, NULL);
}
static void good_fini(void *priv) { if (priv == &g_init) g_fini++; }
static void good_reload(void *priv) { (void)priv; g_reload++; }
static uint32_t good_caps(void *priv) { (void)priv; return 0x5; }

static int bad_init(const struct mm_host *h, void **priv)
{
	(void)priv;
	h->register_verb(h, "orphan", "", ok_verb, NULL);
	return -1;
}
static void bad_fini(void *priv) { (void)priv; g_fini += 100; }

static const struct mm_plugin good = {
	.abi = MM_ABI, .attrs = MM_ATTR_NETWORK, .name = "good", .version = "1",
	.init = good_init, .fini = good_fini, .reload = good_reload, .caps = good_caps,
};
static const struct mm_plugin good_again = { .abi = MM_ABI, .name = "good", .init = good_init };
static const struct mm_plugin old_abi = { .abi = MM_ABI + 1, .name = "future", .init = good_init };
static const struct mm_plugin bad_name = { .abi = MM_ABI, .name = "has.dot", .init = good_init };
static const struct mm_plugin no_init = { .abi = MM_ABI, .name = "noinit" };
static const struct mm_plugin failing = { .abi = MM_ABI, .name = "failing", .init = bad_init, .fini = bad_fini };
static const struct mm_plugin off = { .abi = MM_ABI, .name = "off", .init = good_init };

static int registry(void)
{
	CHECK(loader_add(&good, NULL, "builtin") == 0, "a well-formed plugin is accepted");
	CHECK(loader_add(&good_again, NULL, "x.so") != 0, "a duplicate name is refused");
	CHECK(loader_add(&old_abi, NULL, "x.so") != 0, "an ABI mismatch is refused");
	CHECK(loader_add(&bad_name, NULL, "x.so") != 0, "a name with '.' is refused (it would break config prefixes)");
	CHECK(loader_add(&no_init, NULL, "x.so") != 0, "a plugin without init is refused");
	CHECK(loader_add(NULL, NULL, "x.so") != 0, "a NULL entry result is refused");
	CHECK(loader_add(&failing, NULL, "builtin") == 0, "a plugin that will fail init still loads");
	CHECK(loader_add(&off, NULL, "builtin") == 0, "a disabled plugin is still listed");
	CHECK(loader_state("off") == MM_PLUGIN_DISABLED, "host.disable marks it disabled");
	CHECK(loader_count() == 3, "only accepted plugins count");
	return 0;
}

static int dlopen_scan(const char *dir)
{
	size_t before = loader_count();
	loader_scan("/nonexistent:");
	CHECK(loader_count() == before, "a missing directory adds nothing");
	loader_scan(dir);
	CHECK(loader_state("null") == MM_PLUGIN_LOADED, "null.so is found on the plugin path and loaded");
	size_t after = loader_count();
	CHECK(after > before, "the scan added shared objects");
	loader_scan(dir);
	CHECK(loader_count() == after, "a second scan does not load duplicates");
	return 0;
}

static int lifecycle(void)
{
	loader_init_all();
	CHECK(loader_state("good") == MM_PLUGIN_ACTIVE && g_init == 1, "init runs once and the plugin goes active");
	CHECK(ipc_has_verb("good"), "a verb registered in init is live");
	CHECK(loader_state("failing") == MM_PLUGIN_FAILED, "a failing init marks the plugin failed");
	CHECK(!ipc_has_verb("orphan"), "verbs from a failed init are removed");
	CHECK(loader_state("off") == MM_PLUGIN_DISABLED && g_init == 1, "a disabled plugin is never initialised");
	CHECK(loader_state("null") == MM_PLUGIN_ACTIVE && ipc_has_verb("null"), "the dlopened plugin initialises and registers its verb");

	char desc[2048];
	loader_describe(desc, sizeof(desc));
	CHECK(strstr(desc, "good") && strstr(desc, "attrs=network") && strstr(desc, "caps=0x5"), "describe shows attrs and live caps");
	CHECK(strstr(desc, "failing") && strstr(desc, "failed"), "describe shows failures");
	CHECK(strstr(desc, "null.so"), "describe shows where a plugin came from");

	loader_reload_all();
	CHECK(g_reload == 1, "reload reaches active plugins");

	loader_fini_all();
	CHECK(g_fini == 1, "fini runs for active plugins only, with their own priv");
	CHECK(!ipc_has_verb("good") && !ipc_has_verb("null"), "fini drops every plugin's verbs");
	CHECK(loader_count() == 0, "fini empties the table");
	return 0;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "build/plugins";
	config_override("good.colour=teal");
	config_override("host.disable=other, off");
	config_load(NULL);
	RUN(registry);
	if (dlopen_scan(dir) != 0) return 1;
	RUN(lifecycle);
	config_free();
	CHECK_PASS("test-loader");
}

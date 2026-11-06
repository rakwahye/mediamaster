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
/* The fit rule: same major, built for a minor no newer than the host's. */
static const struct mm_plugin newer_minor = { .abi = MM_ABI + 1, .name = "future", .init = good_init };
static const struct mm_plugin other_major = { .abi = MM_ABI_MAKE(MM_ABI_MAJOR + 1, 0), .name = "next", .init = good_init };
static const struct mm_plugin no_point = { .abi = MM_ABI_MAJOR, .name = "ancient", .init = good_init };
static int plain_init(const struct mm_host *h, void **priv) { (void)h; *priv = NULL; return 0; }
static const struct mm_plugin older_minor = { .abi = MM_ABI_MAKE(MM_ABI_MAJOR, 0), .name = "older", .init = plain_init };
/* Ops tables say their size; one that does not, or lacks a required op, is refused. */
static long nop_read(void *s, float *d, uint32_t n) { (void)s; (void)d; (void)n; return 0; }
static int nop_open(void *p, const char *u, void **s, struct mm_stream_info *i) { (void)p; (void)u; (void)s; (void)i; return -1; }
static void nop_close(void *s) { (void)s; }
static const struct mm_source_ops unsized_ops = { .open = nop_open, .read = nop_read, .close = nop_close };
static const struct mm_source_ops noread_ops = { .size = sizeof(struct mm_source_ops), .open = nop_open, .close = nop_close };
static const struct mm_plugin unsized = { .abi = MM_ABI, .name = "unsized", .schemes = "u", .init = good_init, .source = &unsized_ops };
static const struct mm_plugin noread = { .abi = MM_ABI, .name = "noread", .schemes = "n", .init = good_init, .source = &noread_ops };
static const struct mm_plugin bad_name = { .abi = MM_ABI, .name = "has.dot", .init = good_init };
static const struct mm_plugin no_init = { .abi = MM_ABI, .name = "noinit" };
static const struct mm_plugin failing = { .abi = MM_ABI, .name = "failing", .init = bad_init, .fini = bad_fini };
static const struct mm_plugin off = { .abi = MM_ABI, .name = "off", .init = good_init };

/* The rule itself, with numbers the current SDK does not have. */
static int fit_rule(void)
{
	uint32_t v30 = MM_ABI_MAKE(3, 0), v31 = MM_ABI_MAKE(3, 1), v40 = MM_ABI_MAKE(4, 0);
	CHECK(MM_ABI_MAJOR_OF(v31) == 3 && MM_ABI_MINOR_OF(v31) == 1, "major and minor come back out");
	CHECK(MM_ABI_FITS(v30, v30), "the same version fits");
	CHECK(MM_ABI_FITS(v30, v31), "built for 3.0, offered 3.1: fits (an addition it ignores)");
	CHECK(!MM_ABI_FITS(v31, v30), "built for 3.1, offered 3.0: does not (it may want the addition)");
	CHECK(!MM_ABI_FITS(v30, v40) && !MM_ABI_FITS(v40, v30), "another major never fits, either way");
	CHECK(MM_ABI_MAJOR_OF(3) == 0, "a number from before the point reads as major 0");
	return 0;
}

static int registry(void)
{
	CHECK(loader_add(&good, NULL, "builtin") == 0, "a well-formed plugin is accepted");
	CHECK(loader_add(&good_again, NULL, "x.so") != 0, "a duplicate name is refused");
	CHECK(loader_add(&newer_minor, NULL, "x.so") != 0, "built for a newer minor than the host offers: refused");
	CHECK(loader_add(&other_major, NULL, "x.so") != 0, "another major: refused");
	CHECK(loader_add(&no_point, NULL, "x.so") != 0, "a number from before the point: refused");
	CHECK(loader_add(&unsized, NULL, "x.so") != 0, "an ops table without its size: refused");
	CHECK(loader_add(&noread, NULL, "x.so") != 0, "a source without read: refused");
	CHECK(loader_add(&bad_name, NULL, "x.so") != 0, "a name with '.' is refused (it would break config prefixes)");
	CHECK(loader_add(&no_init, NULL, "x.so") != 0, "a plugin without init is refused");
	CHECK(loader_add(NULL, NULL, "x.so") != 0, "a NULL entry result is refused");
	CHECK(loader_add(&failing, NULL, "builtin") == 0, "a plugin that will fail init still loads");
	CHECK(loader_add(&off, NULL, "builtin") == 0, "a disabled plugin is still listed");
	CHECK(loader_state("off") == MM_PLUGIN_DISABLED, "host.disable marks it disabled");
	CHECK(loader_add(&older_minor, NULL, "builtin") == 0, "built for an older minor of this major: accepted");
	CHECK(loader_count() == 4, "only accepted plugins count");
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

/* Shutdown order: control plugins first, sources and sinks after. */
static char g_order[64];
static int noop_init(const struct mm_host *h, void **priv) { (void)h; *priv = NULL; return 0; }
static void ctl_fini(void *priv) { (void)priv; strcat(g_order, "ctl "); }
static void src_fini(void *priv) { (void)priv; strcat(g_order, "src "); }
static void snk_fini(void *priv) { (void)priv; strcat(g_order, "snk "); }
static int src_open(void *priv, const char *uri, void **st, struct mm_stream_info *in)
{ (void)priv; (void)uri; (void)st; (void)in; return -1; }
static long src_read(void *st, float *dst, uint32_t frames) { (void)st; (void)dst; (void)frames; return 0; }
static void src_close(void *st) { (void)st; }
static int snk_start(void *priv, mm_pull_fn pull, void *ctx) { (void)priv; (void)pull; (void)ctx; return 0; }
static void snk_stop(void *priv) { (void)priv; }
static const struct mm_source_ops src_ops = { .size = sizeof(struct mm_source_ops), .open = src_open, .read = src_read, .close = src_close };
static const struct mm_sink_ops snk_ops = { .size = sizeof(struct mm_sink_ops), .start = snk_start, .stop = snk_stop };
static const struct mm_plugin p_src = { .abi = MM_ABI, .name = "osrc", .schemes = "osrc", .init = noop_init, .fini = src_fini, .source = &src_ops };
static const struct mm_plugin p_ctl = { .abi = MM_ABI, .name = "octl", .init = noop_init, .fini = ctl_fini };
static const struct mm_plugin p_snk = { .abi = MM_ABI, .name = "osnk", .init = noop_init, .fini = snk_fini, .sink = &snk_ops };

static int shutdown_order(void)
{
	g_order[0] = '\0';
	CHECK(loader_add(&p_src, NULL, "test") == 0 && loader_add(&p_ctl, NULL, "test") == 0 &&
	      loader_add(&p_snk, NULL, "test") == 0, "add");
	loader_init_all();
	loader_fini_controls();
	CHECK(!strcmp(g_order, "ctl "), "control plugins alone first");
	CHECK(loader_state("octl") != MM_PLUGIN_ACTIVE && loader_state("osrc") == MM_PLUGIN_ACTIVE, "sources stay up");
	loader_fini_all();
	CHECK(!strcmp(g_order, "ctl snk src "), "then the rest, each fini once");
	return 0;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "build/plugins";
	config_override("good.colour=teal");
	config_override("host.disable=other, off");
	config_load(NULL);
	RUN(fit_rule);
	RUN(registry);
	if (dlopen_scan(dir) != 0) return 1;
	RUN(lifecycle);
	RUN(shutdown_order);
	config_free();
	CHECK_PASS("test-loader");
}

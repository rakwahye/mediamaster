/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file loader.c
 * @brief Plugin registry, dlopen scan, per-plugin host services. */

#include "loader.h"
#include "config.h"
#include "events.h"
#include "ipc.h"
#include "log.h"
#include "player.h"
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "loader"

struct slot {
	const struct mm_plugin *p;
	void *dl;
	void *priv;
	struct mm_host host;
	int state;
	char origin[512];
};

static struct slot s_slots[MM_MAX_PLUGINS];
static size_t s_n;

/* ------------------------------------------------------------------ host services */

static struct slot *slot_of(const struct mm_host *h) { return (struct slot *)h->ctx; }

static void h_log(const struct mm_host *h, int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mm_vlog(level, slot_of(h)->p->name, fmt, ap);
	va_end(ap);
}

static const char *h_config_get(const struct mm_host *h, const char *key)
{
	char k[256];
	if (!key) return NULL;
	if (snprintf(k, sizeof(k), "%s.%s", slot_of(h)->p->name, key) >= (int)sizeof(k)) return NULL;
	return config_get(k);
}

static int h_register_verb(const struct mm_host *h, const char *verb,
                           const char *help, mm_verb_fn fn, void *priv)
{
	return ipc_register(slot_of(h)->p->name, verb, help, fn, priv);
}

static int h_command(const struct mm_host *h, int argc, const char *const *argv,
                     char *reply, size_t reply_len)
{
	(void)h;
	return ipc_dispatch(argc, (char *const *)argv, NULL, 0, reply, reply_len);
}

static int h_subscribe(const struct mm_host *h, uint32_t mask, mm_event_fn cb, void *priv)
{
	return events_subscribe(slot_of(h)->p->name, mask, cb, priv);
}

static void h_stream_update(const struct mm_host *h, void *stream, const struct mm_stream_info *info)
{
	(void)h;
	player_stream_update(stream, info);
}

static void h_stream_tags(const struct mm_host *h, void *stream, const char *const *kv)
{
	(void)h;
	player_stream_tags(stream, kv);
}

/* ------------------------------------------------------------------ registry */

static int name_ok(const char *n)
{
	if (!n || !*n || strlen(n) > 48) return 0;
	for (const char *c = n; *c; c++)
		if (!(isalnum((unsigned char)*c) || *c == '_' || *c == '-')) return 0;
	return 1;
}

/* Whole-word match in a comma/space separated list. */
static int in_list(const char *list, const char *name)
{
	if (!list) return 0;
	size_t nl = strlen(name);
	for (const char *p = list; *p; ) {
		while (*p == ',' || *p == ' ' || *p == '\t') p++;
		const char *e = p;
		while (*e && *e != ',' && *e != ' ' && *e != '\t') e++;
		if ((size_t)(e - p) == nl && !strncmp(p, name, nl)) return 1;
		p = e;
	}
	return 0;
}

int loader_add(const struct mm_plugin *p, void *dl, const char *origin)
{
	if (!origin) origin = "?";
	if (!p) {
		LOGW(TAG, "%s: entry returned NULL, refused", origin);
		return -1;
	}
	if (p->abi != MM_ABI) {
		LOGW(TAG, "%s: plugin ABI %u, host ABI %u, refused", origin, (unsigned)p->abi, (unsigned)MM_ABI);
		return -1;
	}
	if (!name_ok(p->name)) {
		LOGW(TAG, "%s: invalid plugin name, refused", origin);
		return -1;
	}
	if (!p->init) {
		LOGW(TAG, "%s: %s has no init, refused", origin, p->name);
		return -1;
	}
	for (size_t i = 0; i < s_n; i++) {
		if (!strcmp(s_slots[i].p->name, p->name)) {
			LOGI(TAG, "%s: '%s' already provided by %s, skipped", origin, p->name, s_slots[i].origin);
			return -1;
		}
	}
	if (s_n == MM_MAX_PLUGINS) {
		LOGW(TAG, "%s: plugin table full, refused", origin);
		return -1;
	}
	struct slot *s = &s_slots[s_n];
	memset(s, 0, sizeof(*s));
	s->p = p;
	s->dl = dl;
	snprintf(s->origin, sizeof(s->origin), "%s", origin);
	s->host.abi = MM_ABI;
	s->host.size = (uint32_t)sizeof(struct mm_host);
	s->host.log = h_log;
	s->host.config_get = h_config_get;
	s->host.register_verb = h_register_verb;
	s->host.command = h_command;
	s->host.subscribe = h_subscribe;
	s->host.stream_update = h_stream_update;
	s->host.stream_tags = h_stream_tags;
	s->host.ctx = s;
	s->state = in_list(config_get("host.disable"), p->name) ? MM_PLUGIN_DISABLED : MM_PLUGIN_LOADED;
	s_n++;
	LOGD(TAG, "loaded %s %s from %s%s", p->name, p->version ? p->version : "?", origin,
	     s->state == MM_PLUGIN_DISABLED ? " (disabled)" : "");
	return 0;
}

static int has_so_suffix(const char *n)
{
	size_t l = strlen(n);
	return l > 3 && !strcmp(n + l - 3, ".so");
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void scan_dir(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) {
		LOGD(TAG, "no plugin dir %s", dir);
		return;
	}
	char *names[MM_MAX_PLUGINS * 2];
	size_t nn = 0;
	struct dirent *de;
	while ((de = readdir(d)) && nn < sizeof(names) / sizeof(names[0])) {
		if (de->d_name[0] == '.' || !has_so_suffix(de->d_name)) continue;
		names[nn] = strdup(de->d_name);
		if (names[nn]) nn++;
	}
	closedir(d);
	qsort(names, nn, sizeof(names[0]), cmp_str);

	for (size_t i = 0; i < nn; i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		free(names[i]);
		void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (!h) {
			LOGW(TAG, "%s", dlerror());
			continue;
		}
		void *sym = dlsym(h, "mm_plugin_entry");
		if (!sym) {
			LOGW(TAG, "%s: no mm_plugin_entry, refused", path);
			dlclose(h);
			continue;
		}
		mm_plugin_entry_fn entry;
		_Static_assert(sizeof(entry) == sizeof(sym), "function and object pointers differ in size");
		memcpy(&entry, &sym, sizeof(entry));
		if (loader_add(entry(), h, path) != 0) dlclose(h);
	}
}

void loader_scan(const char *dirs)
{
	if (!dirs) return;
	char *copy = strdup(dirs);
	if (!copy) return;
	char *save = NULL;
	for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save))
		if (*d) scan_dir(d);
	free(copy);
}

/* ------------------------------------------------------------------ lifecycle */

void loader_init_all(void)
{
	for (size_t i = 0; i < s_n; i++) {
		struct slot *s = &s_slots[i];
		if (s->state != MM_PLUGIN_LOADED) continue;
		s->priv = NULL;
		int rc = s->p->init(&s->host, &s->priv);
		if (rc == 0) {
			s->state = MM_PLUGIN_ACTIVE;
			LOGI(TAG, "%s %s active", s->p->name, s->p->version ? s->p->version : "");
		} else {
			s->state = MM_PLUGIN_FAILED;
			ipc_unregister_owner(s->p->name); /* anything it registered before failing */
			events_unsubscribe_owner(s->p->name);
			LOGW(TAG, "%s: init failed (%d), continuing without it", s->p->name, rc);
		}
	}
}

void loader_reload_all(void)
{
	for (size_t i = 0; i < s_n; i++) {
		struct slot *s = &s_slots[i];
		if (s->state == MM_PLUGIN_ACTIVE && s->p->reload) s->p->reload(s->priv);
	}
}

void loader_fini_controls(void)
{
	for (size_t i = s_n; i-- > 0; ) {
		struct slot *s = &s_slots[i];
		if (s->state != MM_PLUGIN_ACTIVE || s->p->source || s->p->sink) continue;
		events_unsubscribe_owner(s->p->name);
		if (s->p->fini) s->p->fini(s->priv);
		ipc_unregister_owner(s->p->name);
		s->state = MM_PLUGIN_DISABLED; /* loader_fini_all only unloads it */
	}
}

void loader_fini_all(void)
{
	for (size_t i = s_n; i-- > 0; ) {
		struct slot *s = &s_slots[i];
		events_unsubscribe_owner(s->p->name); /* no callbacks into a plugin being torn down */
		if (s->state == MM_PLUGIN_ACTIVE && s->p->fini) s->p->fini(s->priv);
		ipc_unregister_owner(s->p->name);
		if (s->dl) dlclose(s->dl);
		memset(s, 0, sizeof(*s));
	}
	s_n = 0;
}

int loader_find_source(const char *scheme, const struct mm_plugin **p, void **priv)
{
	for (size_t i = 0; i < s_n; i++) {
		const struct slot *s = &s_slots[i];
		if (s->state != MM_PLUGIN_ACTIVE || !s->p->source || !s->p->schemes) continue;
		if (in_list(s->p->schemes, scheme)) {
			*p = s->p;
			*priv = s->priv;
			return 0;
		}
	}
	return -1;
}

int loader_find_catalog(const char *scheme, const struct mm_plugin **p, void **priv)
{
	for (size_t i = 0; i < s_n; i++) {
		const struct slot *s = &s_slots[i];
		if (s->state != MM_PLUGIN_ACTIVE || !s->p->catalog || !s->p->catalog->list || !s->p->schemes) continue;
		if (in_list(s->p->schemes, scheme)) {
			*p = s->p;
			*priv = s->priv;
			return 0;
		}
	}
	return -1;
}

int loader_find_match(const char *name, const struct mm_plugin **p, void **priv)
{
	for (size_t i = 0; i < s_n; i++) {
		const struct slot *s = &s_slots[i];
		if (s->state == MM_PLUGIN_ACTIVE && s->p->catalog && s->p->catalog->match && !strcmp(s->p->name, name)) {
			*p = s->p;
			*priv = s->priv;
			return 0;
		}
	}
	return -1;
}

int loader_find_radio(const char *name, const struct mm_plugin **p, void **priv)
{
	for (size_t i = 0; i < s_n; i++) {
		const struct slot *s = &s_slots[i];
		if (s->state == MM_PLUGIN_ACTIVE && s->p->catalog && s->p->catalog->radio && !strcmp(s->p->name, name)) {
			*p = s->p;
			*priv = s->priv;
			return 0;
		}
	}
	return -1;
}

void loader_each_radio(void (*fn)(void *ctx, const char *name), void *ctx)
{
	for (size_t i = 0; i < s_n; i++) {
		const struct slot *s = &s_slots[i];
		if (s->state == MM_PLUGIN_ACTIVE && s->p->catalog && s->p->catalog->radio) fn(ctx, s->p->name);
	}
}

void loader_each_catalog(void (*fn)(void *ctx, const char *scheme, const char *name), void *ctx)
{
	for (size_t i = 0; i < s_n; i++) {
		const struct slot *s = &s_slots[i];
		if (s->state != MM_PLUGIN_ACTIVE || !s->p->catalog || !s->p->catalog->list || !s->p->schemes) continue;
		char scheme[32];
		const char *p = s->p->schemes;
		while (*p == ',' || *p == ' ' || *p == '\t') p++;
		size_t n = strcspn(p, ", \t");
		if (!n || n >= sizeof(scheme)) continue;
		memcpy(scheme, p, n);
		scheme[n] = '\0';
		fn(ctx, scheme, s->p->name);
	}
}

int loader_find_sink(const char *name, const struct mm_plugin **p, void **priv)
{
	for (size_t i = 0; i < s_n; i++) {
		const struct slot *s = &s_slots[i];
		if (s->state == MM_PLUGIN_ACTIVE && s->p->sink && !strcmp(s->p->name, name)) {
			*p = s->p;
			*priv = s->priv;
			return 0;
		}
	}
	return -1;
}

int loader_state(const char *name)
{
	for (size_t i = 0; i < s_n; i++)
		if (!strcmp(s_slots[i].p->name, name)) return s_slots[i].state;
	return -1;
}

size_t loader_count(void) { return s_n; }

static const char *state_name(int st)
{
	switch (st) {
	case MM_PLUGIN_LOADED: return "loaded";
	case MM_PLUGIN_ACTIVE: return "active";
	case MM_PLUGIN_FAILED: return "failed";
	case MM_PLUGIN_DISABLED: return "disabled";
	}
	return "?";
}

static size_t list_append(char *buf, size_t cap, size_t off, int *first, const char *word)
{
	if (off >= cap) return off;
	int w = snprintf(buf + off, cap - off, "%s%s", *first ? "" : ",", word);
	*first = 0;
	return w > 0 ? off + (size_t)w : off;
}

static void describe_ifaces(const struct mm_plugin *p, char *buf, size_t cap)
{
	size_t off = 0;
	int first = 1;
	buf[0] = '\0';
	if (p->source)       off = list_append(buf, cap, off, &first, "source");
	if (p->sink)         off = list_append(buf, cap, off, &first, "sink");
	if (p->catalog)      off = list_append(buf, cap, off, &first, "catalog");
	if (p->identify)     off = list_append(buf, cap, off, &first, "identify");
	if (p->enrich)       off = list_append(buf, cap, off, &first, "enrich");
	if (p->observer)     off = list_append(buf, cap, off, &first, "observer");
	if (p->acquire)      off = list_append(buf, cap, off, &first, "acquire");
	if (p->store)        off = list_append(buf, cap, off, &first, "store");
	if (p->orchestrator) off = list_append(buf, cap, off, &first, "orchestrator");
	if (first) snprintf(buf, cap, "-");
}

static void describe_attrs(uint32_t a, char *buf, size_t cap)
{
	size_t off = 0;
	int first = 1;
	buf[0] = '\0';
	if (a & MM_ATTR_NETWORK)        off = list_append(buf, cap, off, &first, "network");
	if (a & MM_ATTR_SPAWNS)         off = list_append(buf, cap, off, &first, "spawns");
	if (a & MM_ATTR_NEEDS_LOGIN)    off = list_append(buf, cap, off, &first, "login");
	if (a & MM_ATTR_RATE_LIMITED)   off = list_append(buf, cap, off, &first, "rate-limited");
	if (a & MM_ATTR_UNOFFICIAL_API) off = list_append(buf, cap, off, &first, "unofficial-api");
	if (first) snprintf(buf, cap, "-");
}

void loader_describe(char *buf, size_t cap)
{
	size_t off = 0;
	if (cap) buf[0] = '\0';
	for (size_t i = 0; i < s_n && off < cap; i++) {
		const struct slot *s = &s_slots[i];
		char ifaces[128], attrs[128];
		describe_ifaces(s->p, ifaces, sizeof(ifaces));
		describe_attrs(s->p->attrs, attrs, sizeof(attrs));
		unsigned caps = (s->state == MM_PLUGIN_ACTIVE && s->p->caps) ? (unsigned)s->p->caps(s->priv) : 0u;
		int w = snprintf(buf + off, cap - off, "%-12s %-8s %-8s ifaces=%s attrs=%s caps=0x%x %s\n",
		                 s->p->name, s->p->version ? s->p->version : "?", state_name(s->state),
		                 ifaces, attrs, caps, s->origin);
		if (w < 0) break;
		off += (size_t)w;
	}
}

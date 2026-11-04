/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file config.c
 * @brief Flat key=value config with sticky overrides and safe reload. */

#include "config.h"
#include "log.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct kv { char *k, *v; };

struct table {
	struct kv *e;
	size_t n, cap;
	struct table *prev; /* older generations, freed at config_free */
};

static pthread_rwlock_t s_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct table *s_cur;
static struct table s_over; /* overrides, applied after the file */
static char *s_path;

static char *xstrdup(const char *s)
{
	char *d = strdup(s);
	if (!d) { LOGE("config", "out of memory"); abort(); }
	return d;
}

static void table_put(struct table *t, const char *k, const char *v)
{
	if (t->n == t->cap) {
		size_t cap = t->cap ? t->cap * 2 : 32;
		struct kv *e = realloc(t->e, cap * sizeof(*e));
		if (!e) { LOGE("config", "out of memory"); abort(); }
		t->e = e;
		t->cap = cap;
	}
	t->e[t->n].k = xstrdup(k);
	t->e[t->n].v = xstrdup(v);
	t->n++;
}

static void table_clear(struct table *t)
{
	for (size_t i = 0; i < t->n; i++) { free(t->e[i].k); free(t->e[i].v); }
	free(t->e);
	t->e = NULL;
	t->n = t->cap = 0;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s)) s++;
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
	return s;
}

/* Split one line. @returns 1 = pair, 0 = blank/comment, -1 = malformed. */
static int parse_line(char *line, char **key, char **val)
{
	char *p = trim(line);
	if (!*p || *p == '#') return 0;

	char *eq = strchr(p, '=');
	if (!eq) return -1;
	*eq = '\0';
	char *k = trim(p);
	if (!*k) return -1;
	for (char *c = k; *c; c++)
		if (isspace((unsigned char)*c)) return -1;

	char *v = eq + 1;
	while (isspace((unsigned char)*v)) v++;
	if (*v == '"') {
		char *end = strchr(v + 1, '"');
		if (!end) return -1;
		*end = '\0';
		v = v + 1;
	} else {
		/* Inline comment: '#' preceded by whitespace. */
		for (char *c = v; *c; c++) {
			if (*c == '#' && c > v && isspace((unsigned char)c[-1])) {
				*c = '\0';
				break;
			}
		}
		v = trim(v);
	}
	*key = k;
	*val = v;
	return 1;
}

static int split_kv(const char *kv, char **k, char **v, char **buf)
{
	*buf = xstrdup(kv);
	int rc = parse_line(*buf, k, v);
	if (rc != 1) { free(*buf); *buf = NULL; return -1; }
	return 0;
}

int config_override(const char *kv)
{
	char *k, *v, *buf;
	if (split_kv(kv, &k, &v, &buf) != 0) return -1;
	pthread_rwlock_wrlock(&s_lock);
	table_put(&s_over, k, v);
	pthread_rwlock_unlock(&s_lock);
	free(buf);
	return 0;
}

int config_load(const char *path)
{
	struct table *t = calloc(1, sizeof(*t));
	if (!t) { LOGE("config", "out of memory"); abort(); }
	int rc = 0;

	if (path && *path) {
		FILE *f = fopen(path, "r");
		if (!f) {
			if (errno == ENOENT) {
				LOGI("config", "%s not found, using defaults", path);
			} else {
				LOGW("config", "%s: %s", path, strerror(errno));
				rc = -1;
			}
		} else {
			char *line = NULL;
			size_t cap = 0;
			unsigned lineno = 0;
			while (getline(&line, &cap, f) >= 0) {
				lineno++;
				char *k, *v;
				int pr = parse_line(line, &k, &v);
				if (pr == 1) table_put(t, k, v);
				else if (pr < 0) LOGW("config", "%s:%u: ignored malformed line", path, lineno);
			}
			free(line);
			fclose(f);
		}
	}

	pthread_rwlock_wrlock(&s_lock);
	for (size_t i = 0; i < s_over.n; i++) table_put(t, s_over.e[i].k, s_over.e[i].v);
	if (path != s_path) {
		char *np = xstrdup(path ? path : "");
		free(s_path);
		s_path = np;
	}
	t->prev = s_cur;
	s_cur = t;
	pthread_rwlock_unlock(&s_lock);
	return rc;
}

int config_reload(void)
{
	return config_load(s_path);
}

const char *config_get(const char *key)
{
	const char *v = NULL;
	if (!key) return NULL;
	pthread_rwlock_rdlock(&s_lock);
	const struct table *t = s_cur;
	if (t) {
		for (size_t i = t->n; i-- > 0; )
			if (!strcmp(t->e[i].k, key)) { v = t->e[i].v; break; }
	} else {
		for (size_t i = s_over.n; i-- > 0; )
			if (!strcmp(s_over.e[i].k, key)) { v = s_over.e[i].v; break; }
	}
	pthread_rwlock_unlock(&s_lock);
	return v;
}

const char *config_get_str(const char *key, const char *def)
{
	const char *v = config_get(key);
	return v ? v : def;
}

long config_get_long(const char *key, long def)
{
	const char *v = config_get(key);
	if (!v || !*v) return def;
	char *end;
	errno = 0;
	long n = strtol(v, &end, 0);
	if (errno || *end) {
		LOGW("config", "%s: not a number: %s", key, v);
		return def;
	}
	return n;
}

int config_get_bool(const char *key, int def)
{
	const char *v = config_get(key);
	if (!v || !*v) return def;
	if (!strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on"))
		return 1;
	if (!strcmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off"))
		return 0;
	LOGW("config", "%s: not a boolean: %s", key, v);
	return def;
}

const char *config_path(void)
{
	return s_path ? s_path : "";
}

void config_free(void)
{
	pthread_rwlock_wrlock(&s_lock);
	while (s_cur) {
		struct table *p = s_cur->prev;
		table_clear(s_cur);
		free(s_cur);
		s_cur = p;
	}
	table_clear(&s_over);
	free(s_path);
	s_path = NULL;
	pthread_rwlock_unlock(&s_lock);
}

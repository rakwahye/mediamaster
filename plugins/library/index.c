/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file index.c
 * @brief Music folder index with tags: saved, refreshed in the
 * background, queried in memory.
 *
 * A snapshot lists folders, each with its path below the root, its
 * modification time, its files (name, size, time, length, tags) and its
 * subfolders. Only the scan thread builds and replaces snapshots; queries
 * read the current one under `lock`. The snapshot a scan starts from
 * stays its reference, read without the lock, which is safe because
 * nothing else replaces or frees it while the scan runs. What a scan
 * shows before it ends (checkpoints) are deep copies.
 *
 * Saved form, one entry per line (names with a line break are skipped):
 *   mmlib 1 <root>
 *   D <sec> <nsec> <folder below root, "" for the root>
 *   f <size> <sec> <nsec> <ms, -1 if not probed yet> <file name>
 *   t <key=value>\t<key=value>...      (after a probed f)
 *   s <subfolder name> */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* versionsort, strverscmp, pipe2, d_type */
#endif
#include "index.h"
#include "probe.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEPTH 16
#define MAX_DIRS 200000
#define CHECKPOINT_S 15
#define PROBE_TIMEOUT_MS 10000
#define TAGS_MAX 2048

struct file {
	char *name;
	long long size, sec, nsec;
	long long ms;   /* -1: not probed yet */
	char *tags;     /* "key=value\n" lines; NULL until probed */
};

struct dir {
	char *rel;
	long long sec, nsec;
	struct file *f;
	size_t nf, capf;
	char **subs;
	size_t ns;
};

struct snap {
	struct dir *v;
	size_t n, cap;
	long files, probed;
};

struct libidx {
	const struct mm_host *h;
	char root[4096], saved[4096], ffprobe[1024];

	pthread_mutex_t lock;   /* everything below */
	struct snap *cur;
	int want, running, stop, checkpoint_s;
	long dirs_read, probes, scans;
	pthread_t th;
	int th_on;
};

/* ------------------------------------------------------------------ names */

int libidx_audio_name(const char *name)
{
	static const char *const ext[] = {
		"flac", "mp3", "ogg", "oga", "opus", "m4a", "m4b", "aac", "wav", "aif", "aiff",
		"wv", "ape", "mka", "mpc", "alac", "wma", "dsf", "dff", "tta", "spx", NULL
	};
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name) return 0;
	for (const char *const *e = ext; *e; e++) {
		const char *a = dot + 1, *b = *e;
		while (*a && *b && (*a | 0x20) == *b) { a++; b++; }
		if (!*a && !*b) return 1;
	}
	return 0;
}

void libidx_tag(const char *tags, const char *key, char *out, size_t cap)
{
	size_t kl = strlen(key);
	out[0] = '\0';
	for (const char *p = tags; p && *p; ) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n > kl && !strncmp(p, key, kl) && p[kl] == '=') {
			snprintf(out, cap, "%.*s", (int)(n - kl - 1), p + kl + 1);
			return;
		}
		p = e ? e + 1 : NULL;
	}
}

/* ------------------------------------------------------------------ snapshots */

static void dir_clear(struct dir *d)
{
	for (size_t k = 0; k < d->nf; k++) {
		free(d->f[k].name);
		free(d->f[k].tags);
	}
	for (size_t k = 0; k < d->ns; k++) free(d->subs[k]);
	free(d->f);
	free(d->subs);
	free(d->rel);
}

static void snap_free(struct snap *s)
{
	if (!s) return;
	for (size_t i = 0; i < s->n; i++) dir_clear(&s->v[i]);
	free(s->v);
	free(s);
}

static struct dir *snap_add(struct snap *s, const char *rel, long long sec, long long nsec)
{
	if (s->n == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 256;
		struct dir *v = realloc(s->v, cap * sizeof(*v));
		if (!v) return NULL;
		s->v = v;
		s->cap = cap;
	}
	struct dir *d = &s->v[s->n];
	memset(d, 0, sizeof(*d));
	if (!(d->rel = strdup(rel))) return NULL;
	d->sec = sec;
	d->nsec = nsec;
	s->n++;
	return d;
}

/* Add a file to d; tags copied if given. Counts go to s. */
static int file_add(struct snap *s, struct dir *d, const char *name, long long size, long long sec, long long nsec,
                    long long ms, const char *tags)
{
	if (d->nf == d->capf) {
		size_t cap = d->capf ? d->capf * 2 : 16;
		struct file *v = realloc(d->f, cap * sizeof(*v));
		if (!v) return -1;
		d->f = v;
		d->capf = cap;
	}
	struct file *f = &d->f[d->nf];
	memset(f, 0, sizeof(*f));
	if (!(f->name = strdup(name))) return -1;
	f->size = size;
	f->sec = sec;
	f->nsec = nsec;
	f->ms = tags ? ms : -1;
	if (tags && !(f->tags = strdup(tags))) {
		free(f->name);
		return -1;
	}
	d->nf++;
	s->files++;
	if (tags) s->probed++;
	return 0;
}

static int sub_add(struct dir *d, const char *name)
{
	char **v = realloc(d->subs, (d->ns + 1) * sizeof(*v));
	if (!v) return -1;
	d->subs = v;
	if (!(v[d->ns] = strdup(name))) return -1;
	d->ns++;
	return 0;
}

static int by_rel(const void *a, const void *b)
{
	return strcmp(((const struct dir *)a)->rel, ((const struct dir *)b)->rel);
}

static const struct dir *find_dir(const struct snap *s, const char *rel)
{
	struct dir key;
	if (!s || !s->n) return NULL;
	key.rel = (char *)rel;
	return bsearch(&key, s->v, s->n, sizeof(*s->v), by_rel);
}

/* The entry for `name` in d, searching from *hint (names come in the same
 * order as last time, so this is nearly always the next one). */
static const struct file *find_file(const struct dir *d, const char *name, size_t *hint)
{
	for (size_t k = 0; d && k < d->nf; k++) {
		size_t i = (*hint + k) % d->nf;
		if (!strcmp(d->f[i].name, name)) {
			*hint = i + 1;
			return &d->f[i];
		}
	}
	return NULL;
}

static int dir_copy(struct snap *s, const struct dir *d)
{
	struct dir *c = snap_add(s, d->rel, d->sec, d->nsec);
	if (!c) return -1;
	for (size_t k = 0; k < d->nf; k++) {
		const struct file *f = &d->f[k];
		if (file_add(s, c, f->name, f->size, f->sec, f->nsec, f->ms, f->tags) != 0) return -1;
	}
	for (size_t k = 0; k < d->ns; k++)
		if (sub_add(c, d->subs[k]) != 0) return -1;
	return 0;
}

/* Every folder of `part`, and those of `old` it has not reached, sorted;
 * a deep copy. NULL if out of memory. */
static struct snap *merged(const struct snap *part, const struct snap *old)
{
	struct snap *m = calloc(1, sizeof(*m));
	if (!m) return NULL;
	for (size_t i = 0; i < part->n; i++)
		if (dir_copy(m, &part->v[i]) != 0) goto oom;
	if (m->n) qsort(m->v, m->n, sizeof(*m->v), by_rel);
	size_t reached = m->n;
	for (size_t i = 0; old && i < old->n; i++) {
		struct dir key;
		key.rel = old->v[i].rel;
		if (reached && bsearch(&key, m->v, reached, sizeof(*m->v), by_rel)) continue;
		if (dir_copy(m, &old->v[i]) != 0) goto oom;
	}
	if (m->n) qsort(m->v, m->n, sizeof(*m->v), by_rel);
	return m;
oom:
	snap_free(m);
	return NULL;
}

/* ------------------------------------------------------------------ saved form */

static struct snap *load(const char *path, const char *root)
{
	FILE *fp = fopen(path, "r");
	char *line, head[4200];
	size_t cap = 16384;
	struct snap *s;
	struct dir *d = NULL;
	if (!fp) return NULL;
	snprintf(head, sizeof(head), "mmlib 1 %s\n", root);
	line = malloc(cap);
	if (!line || !fgets(line, (int)cap, fp) || strcmp(line, head) != 0 || !(s = calloc(1, sizeof(*s)))) {
		free(line);
		fclose(fp);
		return NULL; /* another root or version: start afresh */
	}
	while (fgets(line, (int)cap, fp)) {
		size_t n = strlen(line);
		long long a, b, c, ms;
		int off = 0;
		if (!n || line[n - 1] != '\n') break; /* cut short */
		line[n - 1] = '\0';
		if (line[0] == 'D' && sscanf(line, "D %lld %lld %n", &a, &b, &off) == 2 && off) {
			if (!(d = snap_add(s, line + off, a, b))) break;
		} else if (d && line[0] == 'f' && sscanf(line, "f %lld %lld %lld %lld %n", &a, &b, &c, &ms, &off) == 4 && off) {
			if (file_add(s, d, line + off, a, b, c, -1, NULL) != 0) break;
			d->f[d->nf - 1].ms = ms; /* tags follow if probed */
		} else if (d && d->nf && line[0] == 't' && (line[1] == ' ' || line[1] == '\0')) {
			struct file *f = &d->f[d->nf - 1];
			char *t = strdup(line[1] ? line + 2 : "");
			if (!t) break;
			for (char *p = t; *p; p++)
				if (*p == '\t') *p = '\n';
			size_t tl = strlen(t);
			if (tl) { /* back to one line per tag, each ending in \n */
				char *u = realloc(t, tl + 2);
				if (!u) { free(t); break; }
				t = u;
				t[tl] = '\n';
				t[tl + 1] = '\0';
			}
			free(f->tags);
			f->tags = t;
			if (f->ms < 0) f->ms = 0;
			s->probed++;
		} else if (d && line[0] == 's' && line[1] == ' ') {
			if (sub_add(d, line + 2) != 0) break;
		}
	}
	free(line);
	fclose(fp);
	/* An f with ms >= 0 but no t line was probed with no tags. */
	for (size_t i = 0; i < s->n; i++)
		for (size_t k = 0; k < s->v[i].nf; k++) {
			struct file *f = &s->v[i].f[k];
			if (!f->tags && f->ms >= 0) {
				f->tags = strdup("");
				if (f->tags) s->probed++;
			}
		}
	if (s->n) qsort(s->v, s->n, sizeof(*s->v), by_rel);
	return s;
}

static void save(const struct libidx *x, const struct snap *s)
{
	char tmp[sizeof(x->saved) + 8];
	snprintf(tmp, sizeof(tmp), "%s.tmp", x->saved);
	FILE *fp = fopen(tmp, "w");
	if (!fp) {
		x->h->log(x->h, MM_LOG_INFO, "cannot save the library index to %s: %s", tmp, strerror(errno));
		return;
	}
	fprintf(fp, "mmlib 1 %s\n", x->root);
	for (size_t i = 0; i < s->n; i++) {
		const struct dir *d = &s->v[i];
		fprintf(fp, "D %lld %lld %s\n", d->sec, d->nsec, d->rel);
		for (size_t k = 0; k < d->nf; k++) {
			const struct file *f = &d->f[k];
			fprintf(fp, "f %lld %lld %lld %lld %s\n", f->size, f->sec, f->nsec, f->tags ? f->ms : -1LL, f->name);
			if (f->tags && f->tags[0]) {
				fputs("t ", fp);
				size_t tl = strlen(f->tags);
				for (size_t j = 0; j < tl; j++)
					if (!(j == tl - 1 && f->tags[j] == '\n')) fputc(f->tags[j] == '\n' ? '\t' : f->tags[j], fp);
				fputc('\n', fp);
			}
		}
		for (size_t k = 0; k < d->ns; k++) fprintf(fp, "s %s\n", d->subs[k]);
	}
	if (fclose(fp) != 0 || rename(tmp, x->saved) != 0) unlink(tmp);
}

/* ------------------------------------------------------------------ scan */

struct scan {
	struct libidx *x;
	const struct snap *old;  /* the index the scan started from */
	struct snap *new;
	int shown;               /* a checkpoint has been made current */
	int phase;               /* 1 folders, 2 tags */
	struct timespec last;
	struct seen { dev_t dev; ino_t ino; int used; } *seen; /* folders visited */
	size_t nseen, capseen;
	long read, probes;
	int fail;
};

static int stopping(struct libidx *x)
{
	pthread_mutex_lock(&x->lock);
	int s = x->stop;
	pthread_mutex_unlock(&x->lock);
	return s;
}

static size_t seen_slot(const struct seen *v, size_t cap, dev_t dev, ino_t ino)
{
	size_t i = (size_t)(((unsigned long long)ino * 0x9E3779B97F4A7C15ull) ^ (unsigned long long)dev) & (cap - 1);
	while (v[i].used && !(v[i].dev == dev && v[i].ino == ino)) i = (i + 1) & (cap - 1);
	return i;
}

/* A link back up the tree, or two links to one folder: followed once. */
static int first_visit(struct scan *sc, const struct stat *st)
{
	if (2 * (sc->nseen + 1) > sc->capseen) {
		size_t cap = sc->capseen ? sc->capseen * 2 : 1024;
		struct seen *v = calloc(cap, sizeof(*v));
		if (!v) return 0;
		for (size_t i = 0; i < sc->capseen; i++)
			if (sc->seen[i].used) v[seen_slot(v, cap, sc->seen[i].dev, sc->seen[i].ino)] = sc->seen[i];
		free(sc->seen);
		sc->seen = v;
		sc->capseen = cap;
	}
	size_t i = seen_slot(sc->seen, sc->capseen, st->st_dev, st->st_ino);
	if (sc->seen[i].used) return 0;
	sc->seen[i].dev = st->st_dev;
	sc->seen[i].ino = st->st_ino;
	sc->seen[i].used = 1;
	sc->nseen++;
	return 1;
}

/* Make `s` current; free the one it replaces unless that is the scan's
 * reference. */
static void publish(struct scan *sc, struct snap *s)
{
	pthread_mutex_lock(&sc->x->lock);
	struct snap *prev = sc->x->cur;
	sc->x->cur = s;
	pthread_mutex_unlock(&sc->x->lock);
	if (prev != sc->old) snap_free(prev);
}

static void checkpoint(struct scan *sc)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	pthread_mutex_lock(&sc->x->lock);
	int every = sc->x->checkpoint_s;
	pthread_mutex_unlock(&sc->x->lock);
	if (now.tv_sec - sc->last.tv_sec < every) return;
	sc->last = now;
	struct snap *m = merged(sc->new, sc->phase == 1 ? sc->old : NULL);
	if (!m) return;
	save(sc->x, m);
	publish(sc, m);
	sc->shown = 1;
	if (sc->phase == 1)
		sc->x->h->log(sc->x->h, MM_LOG_INFO, "library: %zu folders so far, saved", sc->new->n);
	else
		sc->x->h->log(sc->x->h, MM_LOG_INFO, "library: tags for %ld of %ld files so far, saved",
		              sc->new->probed, sc->new->files);
}

static int visible(const struct dirent *e)
{
	return e->d_name[0] != '.' && !strchr(e->d_name, '\n');
}

/* One file of folder `path`: stat it; keep what the old index knew if it
 * has not changed. */
static void take_file(struct scan *sc, struct dir *d, const struct dir *od, size_t *hint, const char *path,
                      const char *name)
{
	char full[8192];
	struct stat st;
	if (snprintf(full, sizeof(full), "%s/%s", path, name) >= (int)sizeof(full) || stat(full, &st) != 0 ||
	    !S_ISREG(st.st_mode))
		return;
	long long size = (long long)st.st_size, sec = (long long)st.st_mtim.tv_sec, nsec = (long long)st.st_mtim.tv_nsec;
	const struct file *of = find_file(od, name, hint);
	int same = of && of->size == size && of->sec == sec && of->nsec == nsec && of->tags;
	if (file_add(sc->new, d, name, size, sec, nsec, same ? of->ms : -1, same ? of->tags : NULL) != 0) sc->fail = 1;
}

static void visit(struct scan *sc, const char *rel, int depth)
{
	char path[8192];
	struct stat st;
	if (sc->fail || depth > MAX_DEPTH || sc->new->n >= MAX_DIRS || stopping(sc->x)) return;
	checkpoint(sc);
	snprintf(path, sizeof(path), "%s%s%s", sc->x->root, rel[0] ? "/" : "", rel);
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode) || !first_visit(sc, &st)) return;

	struct dir *d = snap_add(sc->new, rel, (long long)st.st_mtim.tv_sec, (long long)st.st_mtim.tv_nsec);
	if (!d) {
		sc->fail = 1;
		return;
	}
	size_t di = sc->new->n - 1; /* d moves as the snapshot grows */
	const struct dir *od = find_dir(sc->old, rel);
	size_t hint = 0;
	if (od && od->sec == d->sec && od->nsec == d->nsec) {
		/* Unchanged: the same names, no need to read the folder. */
		for (size_t k = 0; k < od->nf && !sc->fail; k++) take_file(sc, d, od, &hint, path, od->f[k].name);
		for (size_t k = 0; k < od->ns && !sc->fail; k++) sc->fail = sub_add(d, od->subs[k]);
	} else {
		struct dirent **ents;
		int n = scandir(path, &ents, visible, versionsort);
		sc->read++;
		for (int i = 0; i < n; i++) {
			const char *name = ents[i]->d_name;
			int is_dir = ents[i]->d_type == DT_DIR, is_reg = ents[i]->d_type == DT_REG;
			if (ents[i]->d_type == DT_UNKNOWN || ents[i]->d_type == DT_LNK) {
				char full[8192];
				struct stat es;
				if (snprintf(full, sizeof(full), "%s/%s", path, name) < (int)sizeof(full) && stat(full, &es) == 0) {
					is_dir = S_ISDIR(es.st_mode);
					is_reg = S_ISREG(es.st_mode);
				}
			}
			if (!sc->fail && is_dir) sc->fail = sub_add(d, name);
			else if (!sc->fail && is_reg && libidx_audio_name(name)) take_file(sc, d, od, &hint, path, name);
			free(ents[i]);
		}
		if (n >= 0) free(ents);
	}
	d = &sc->new->v[di];
	size_t ns = d->ns;
	for (size_t k = 0; k < ns && !sc->fail; k++) {
		char sub[8192];
		d = &sc->new->v[di];
		snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", d->subs[k]);
		visit(sc, sub, depth + 1);
	}
}

/* Phase 2: probe every file not probed yet. */
static void probe_all(struct scan *sc)
{
	char path[12288], tags[TAGS_MAX];
	for (size_t i = 0; i < sc->new->n && !sc->fail; i++) {
		for (size_t k = 0; k < sc->new->v[i].nf; k++) {
			struct dir *d = &sc->new->v[i];
			struct file *f = &d->f[k];
			if (f->tags) continue;
			if (stopping(sc->x)) return;
			checkpoint(sc);
			snprintf(path, sizeof(path), "%s%s%s/%s", sc->x->root, d->rel[0] ? "/" : "", d->rel, f->name);
			long long ms = probe_file(sc->x->ffprobe, path, PROBE_TIMEOUT_MS, tags, sizeof(tags));
			if (ms < 0) { /* no ffprobe at all: leave tags for another time */
				sc->x->h->log(sc->x->h, MM_LOG_WARN, "cannot run %s: no tags", sc->x->ffprobe);
				return;
			}
			if (!(f->tags = strdup(tags))) {
				sc->fail = 1;
				return;
			}
			f->ms = ms;
			sc->new->probed++;
			sc->probes++;
		}
	}
}

static void *scanner(void *arg)
{
	struct libidx *x = arg;
	for (;;) {
		pthread_mutex_lock(&x->lock);
		if (x->stop || !x->want) {
			x->running = 0;
			pthread_mutex_unlock(&x->lock);
			return NULL;
		}
		x->want = 0;
		pthread_mutex_unlock(&x->lock);

		struct timespec t0, t1;
		struct scan sc;
		memset(&sc, 0, sizeof(sc));
		sc.x = x;
		sc.old = x->cur; /* only this thread replaces it */
		sc.phase = 1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		sc.last = t0;
		x->h->log(x->h, MM_LOG_INFO, "library: updating %s (%zu folders, %ld files saved)", x->root,
		          sc.old ? sc.old->n : (size_t)0, sc.old ? sc.old->files : 0L);
		if ((sc.new = calloc(1, sizeof(*sc.new)))) visit(&sc, "", 0);
		free(sc.seen);
		int done = sc.new && !sc.fail && !stopping(x);
		if (done) {
			if (sc.new->n) qsort(sc.new->v, sc.new->n, sizeof(*sc.new->v), by_rel);
			/* Folders are complete: show them before the long tag pass. */
			struct snap *m = merged(sc.new, NULL);
			if (m) {
				publish(&sc, m);
				sc.shown = 1;
			}
			sc.phase = 2;
			probe_all(&sc);
			done = !sc.fail && !stopping(x);
		}
		if (!done) {
			if (sc.new && !sc.fail) { /* stopped: keep the progress */
				struct snap *m = merged(sc.new, sc.phase == 1 ? sc.old : NULL);
				if (m) save(x, m);
				snap_free(m);
			}
			snap_free(sc.new);
			if (sc.shown) snap_free((struct snap *)sc.old); /* a checkpoint is current now */
			continue;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		pthread_mutex_lock(&x->lock);
		x->dirs_read = sc.read;
		x->probes = sc.probes;
		x->scans++;
		pthread_mutex_unlock(&x->lock);
		x->h->log(x->h, MM_LOG_INFO, "library: %ld files (%ld with tags) in %zu folders; %ld read, %ld probed, in %ld ms",
		          sc.new->files, sc.new->probed, sc.new->n, sc.read, sc.probes,
		          (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
		int changed = sc.read || sc.probes || !sc.old || sc.old->n != sc.new->n || sc.old->files != sc.new->files;
		if (changed || sc.shown) save(x, sc.new);
		publish(&sc, sc.new);
		snap_free((struct snap *)sc.old);
	}
}

/* ------------------------------------------------------------------ api */

struct libidx *libidx_open(const struct mm_host *h, const char *root, const char *saved, const char *ffprobe)
{
	struct libidx *x = calloc(1, sizeof(*x));
	if (!x) return NULL;
	x->h = h;
	snprintf(x->root, sizeof(x->root), "%s", root);
	size_t n = strlen(x->root);
	while (n > 1 && x->root[n - 1] == '/') x->root[--n] = '\0';
	snprintf(x->saved, sizeof(x->saved), "%s", saved);
	snprintf(x->ffprobe, sizeof(x->ffprobe), "%s", ffprobe);
	pthread_mutex_init(&x->lock, NULL);
	x->checkpoint_s = CHECKPOINT_S;
	x->cur = load(x->saved, x->root);
	libidx_rescan(x);
	return x;
}

void libidx_checkpoint_every(struct libidx *x, int seconds)
{
	pthread_mutex_lock(&x->lock);
	x->checkpoint_s = seconds < 0 ? 0 : seconds;
	pthread_mutex_unlock(&x->lock);
}

void libidx_rescan(struct libidx *x)
{
	pthread_mutex_lock(&x->lock);
	x->want = 1;
	if (!x->running && !x->stop) {
		if (x->th_on) pthread_join(x->th, NULL); /* a finished scan */
		x->th_on = pthread_create(&x->th, NULL, scanner, x) == 0;
		x->running = x->th_on;
	}
	pthread_mutex_unlock(&x->lock);
}

void libidx_each(struct libidx *x, int (*fn)(void *ctx, const struct libidx_track *t), void *ctx)
{
	char path[12288], rel[8192];
	pthread_mutex_lock(&x->lock);
	const struct snap *s = x->cur;
	int stop = 0;
	for (size_t i = 0; s && i < s->n && !stop; i++) {
		const struct dir *d = &s->v[i];
		for (size_t k = 0; k < d->nf && !stop; k++) {
			const struct file *f = &d->f[k];
			snprintf(rel, sizeof(rel), "%s%s%s", d->rel, d->rel[0] ? "/" : "", f->name);
			snprintf(path, sizeof(path), "%s/%s", x->root, rel);
			struct libidx_track t = { path, rel, f->name, f->tags ? f->tags : "", f->tags != NULL,
			                          f->ms > 0 ? (uint64_t)f->ms : 0 };
			stop = fn(ctx, &t) != 0;
		}
	}
	pthread_mutex_unlock(&x->lock);
}

void libidx_stats(struct libidx *x, struct libidx_stats *out)
{
	pthread_mutex_lock(&x->lock);
	memset(out, 0, sizeof(*out));
	if (x->cur) {
		out->dirs = (long)x->cur->n;
		out->files = x->cur->files;
		out->probed = x->cur->probed;
	}
	out->dirs_read = x->dirs_read;
	out->probes = x->probes;
	out->scanning = x->running;
	out->scans = x->scans;
	pthread_mutex_unlock(&x->lock);
}

void libidx_close(struct libidx *x)
{
	if (!x) return;
	pthread_mutex_lock(&x->lock);
	x->stop = 1;
	pthread_mutex_unlock(&x->lock);
	if (x->th_on) pthread_join(x->th, NULL);
	snap_free(x->cur);
	pthread_mutex_destroy(&x->lock);
	free(x);
}

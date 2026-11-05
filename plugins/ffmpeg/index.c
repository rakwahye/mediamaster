/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file index.c
 * @brief Music folder index: saved, refreshed folder by folder, searched
 * in memory.
 *
 * A snapshot is a list of folders, each with its path below the root, its
 * modification time, its audio files and its subfolders. Only the scan
 * thread builds and replaces snapshots; searches read the current one
 * under `lock`. The previous snapshot stays the scan's reference, read
 * without the lock, which is safe because nothing else replaces or frees
 * it.
 *
 * A first scan of a big or slow drive takes minutes. So that it is never
 * lost or invisible, the scan checkpoints every CHECKPOINT_S: what it has
 * scanned, plus what the previous index knew of the folders it has not
 * reached yet, becomes the current index and is saved. Stopping
 * mid-scan saves the same, and the next scan carries on from there.
 *
 * Saved form, one entry per line (names with a line break are skipped):
 *   mmidx 1 <root>
 *   D <sec> <nsec> <path below root, "" for the root>
 *   f <audio file name>
 *   s <subfolder name> */

#include "index.h"
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
#define CHECKPOINT_S 15 /* default */

struct dir {
	char *rel;
	long long sec, nsec;
	char **files, **subs;
	size_t nfiles, nsubs;
};

struct snap {
	struct dir *v;
	size_t n, cap;
	long files;
};

struct ffidx {
	const struct mm_host *h;
	char root[4096], saved[4096];

	pthread_mutex_t lock;   /* cur, the stats, want, running, stop */
	struct snap *cur;
	int want, running, stop;
	int checkpoint_s;
	long dirs_read, scans;
	pthread_t th;
	int th_on;
};

/* ------------------------------------------------------------------ names */

int ffidx_audio_name(const char *name)
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

/* ------------------------------------------------------------------ snapshots */

static void snap_free(struct snap *s)
{
	if (!s) return;
	for (size_t i = 0; i < s->n; i++) {
		struct dir *d = &s->v[i];
		for (size_t k = 0; k < d->nfiles; k++) free(d->files[k]);
		for (size_t k = 0; k < d->nsubs; k++) free(d->subs[k]);
		free(d->files);
		free(d->subs);
		free(d->rel);
	}
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

static int push(char ***v, size_t *n, const char *name)
{
	char **w = realloc(*v, (*n + 1) * sizeof(**v));
	if (!w) return -1;
	*v = w;
	if (!(w[*n] = strdup(name))) return -1;
	(*n)++;
	return 0;
}

static int by_rel(const void *a, const void *b)
{
	return strcmp(((const struct dir *)a)->rel, ((const struct dir *)b)->rel);
}

/* The folder `rel` in a snapshot sorted by rel, or NULL. */
static const struct dir *find_dir(const struct snap *s, const char *rel)
{
	struct dir key;
	if (!s || !s->n) return NULL;
	key.rel = (char *)rel;
	return bsearch(&key, s->v, s->n, sizeof(*s->v), by_rel);
}

/* Deep copy of d into s. */
static int dir_copy(struct snap *s, const struct dir *d)
{
	struct dir *c = snap_add(s, d->rel, d->sec, d->nsec);
	if (!c) return -1;
	for (size_t k = 0; k < d->nfiles; k++)
		if (push(&c->files, &c->nfiles, d->files[k]) != 0) return -1;
	for (size_t k = 0; k < d->nsubs; k++)
		if (push(&c->subs, &c->nsubs, d->subs[k]) != 0) return -1;
	s->files += (long)d->nfiles;
	return 0;
}

/* What a scan knows so far: every folder it has reached (`part`, in scan
 * order), and from `old` those it has not, sorted. NULL if out of memory. */
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
	FILE *f = fopen(path, "r");
	char line[8192], head[sizeof(line)];
	struct snap *s;
	struct dir *d = NULL;
	if (!f) return NULL;
	snprintf(head, sizeof(head), "mmidx 1 %s\n", root);
	if (!fgets(line, sizeof(line), f) || strcmp(line, head) != 0 || !(s = calloc(1, sizeof(*s)))) {
		fclose(f);
		return NULL; /* another root, another version: start afresh */
	}
	while (fgets(line, sizeof(line), f)) {
		size_t n = strlen(line);
		long long sec, nsec;
		int off = 0;
		if (!n || line[n - 1] != '\n') break; /* cut short */
		line[n - 1] = '\0';
		if (line[0] == 'D' && sscanf(line, "D %lld %lld %n", &sec, &nsec, &off) == 2 && off) {
			if (!(d = snap_add(s, line + off, sec, nsec))) break;
		} else if (d && line[0] == 'f' && line[1] == ' ') {
			if (push(&d->files, &d->nfiles, line + 2) != 0) break;
			s->files++;
		} else if (d && line[0] == 's' && line[1] == ' ') {
			if (push(&d->subs, &d->nsubs, line + 2) != 0) break;
		}
	}
	fclose(f);
	if (s->n) qsort(s->v, s->n, sizeof(*s->v), by_rel);
	return s;
}

static void save(const struct ffidx *x, const struct snap *s)
{
	char tmp[sizeof(x->saved) + 8];
	snprintf(tmp, sizeof(tmp), "%s.tmp", x->saved);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		x->h->log(x->h, MM_LOG_INFO, "cannot save the index to %s: %s", tmp, strerror(errno));
		return;
	}
	fprintf(f, "mmidx 1 %s\n", x->root);
	for (size_t i = 0; i < s->n; i++) {
		const struct dir *d = &s->v[i];
		fprintf(f, "D %lld %lld %s\n", d->sec, d->nsec, d->rel);
		for (size_t k = 0; k < d->nfiles; k++) fprintf(f, "f %s\n", d->files[k]);
		for (size_t k = 0; k < d->nsubs; k++) fprintf(f, "s %s\n", d->subs[k]);
	}
	if (fclose(f) != 0 || rename(tmp, x->saved) != 0) unlink(tmp);
}

/* ------------------------------------------------------------------ scan */

struct scan {
	struct ffidx *x;
	const struct snap *old; /* the index the scan started from */
	struct snap *new;
	struct snap *shown;     /* the last checkpoint made current, if any */
	struct timespec last;   /* when it was made */
	/* Folders visited, an open-addressing set of (device, inode): a link
	 * back up the tree, or two links to one folder, is followed once. */
	struct seen { dev_t dev; ino_t ino; int used; } *seen;
	size_t nseen, capseen;
	long read;
	int fail;
};

static int stopping(struct ffidx *x)
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

static int first_visit(struct scan *sc, const struct stat *st)
{
	if (2 * (sc->nseen + 1) > sc->capseen) { /* keep it at most half full */
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

static int visible(const struct dirent *e)
{
	return e->d_name[0] != '.' && !strchr(e->d_name, '\n');
}

/* Read a folder's entries into d: audio files and subfolders. */
static int read_dir(struct scan *sc, const char *path, struct dir *d)
{
	struct dirent **ents;
	int n = scandir(path, &ents, visible, versionsort);
	if (n < 0) return 0; /* unreadable: indexed as empty */
	sc->read++;
	for (int i = 0; i < n; i++) {
		char full[8192];
		struct stat st;
		int is_dir = ents[i]->d_type == DT_DIR, is_reg = ents[i]->d_type == DT_REG;
		if ((ents[i]->d_type == DT_UNKNOWN || ents[i]->d_type == DT_LNK) &&
		    snprintf(full, sizeof(full), "%s/%s", path, ents[i]->d_name) < (int)sizeof(full) &&
		    stat(full, &st) == 0) {
			is_dir = S_ISDIR(st.st_mode);
			is_reg = S_ISREG(st.st_mode);
		}
		if (is_dir && push(&d->subs, &d->nsubs, ents[i]->d_name) != 0) sc->fail = 1;
		else if (is_reg && ffidx_audio_name(ents[i]->d_name)) {
			if (push(&d->files, &d->nfiles, ents[i]->d_name) != 0) sc->fail = 1;
			else sc->new->files++;
		}
		free(ents[i]);
	}
	free(ents);
	return 0;
}

/* Every CHECKPOINT_S: make what is known so far current, and save it. */
static void checkpoint(struct scan *sc)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	pthread_mutex_lock(&sc->x->lock);
	int every = sc->x->checkpoint_s;
	pthread_mutex_unlock(&sc->x->lock);
	if (now.tv_sec - sc->last.tv_sec < every) return;
	sc->last = now;
	struct snap *m = merged(sc->new, sc->old);
	if (!m) return;
	save(sc->x, m);
	pthread_mutex_lock(&sc->x->lock);
	struct snap *prev = sc->x->cur;
	sc->x->cur = m;
	pthread_mutex_unlock(&sc->x->lock);
	if (prev != sc->old) snap_free(prev); /* an earlier checkpoint */
	sc->shown = m;
	sc->x->h->log(sc->x->h, MM_LOG_INFO, "music index: %zu folders so far, saved", sc->new->n);
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
	const struct dir *od = find_dir(sc->old, rel);
	if (od && od->sec == d->sec && od->nsec == d->nsec) {
		for (size_t k = 0; k < od->nfiles && !sc->fail; k++) sc->fail = push(&d->files, &d->nfiles, od->files[k]);
		for (size_t k = 0; k < od->nsubs && !sc->fail; k++) sc->fail = push(&d->subs, &d->nsubs, od->subs[k]);
		sc->new->files += (long)od->nfiles;
	} else {
		read_dir(sc, path, d);
	}
	/* d may move as the snapshot grows: take what the recursion needs. */
	size_t nsubs = d->nsubs;
	char **subs = d->subs;
	char sub[8192];
	for (size_t k = 0; k < nsubs; k++) {
		snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", subs[k]);
		visit(sc, sub, depth + 1);
	}
}

static void *scanner(void *arg)
{
	struct ffidx *x = arg;
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
		clock_gettime(CLOCK_MONOTONIC, &t0);
		sc.last = t0;
		x->h->log(x->h, MM_LOG_INFO, "music index: updating %s (%zu folders saved)", x->root,
		          sc.old ? sc.old->n : (size_t)0);
		if ((sc.new = calloc(1, sizeof(*sc.new)))) visit(&sc, "", 0);
		free(sc.seen);
		if (sc.new && !sc.fail && stopping(x)) {
			/* Stopped: keep the progress for next time. */
			struct snap *m = merged(sc.new, sc.old);
			if (m) save(x, m);
			snap_free(m);
		}
		if (!sc.new || sc.fail || stopping(x)) {
			snap_free(sc.new);
			/* The index stays as it is: the old one, or a checkpoint that
			 * holds all it knew, which then replaces it for good. */
			if (sc.shown) snap_free((struct snap *)sc.old);
			continue;
		}
		if (sc.new->n) qsort(sc.new->v, sc.new->n, sizeof(*sc.new->v), by_rel);
		clock_gettime(CLOCK_MONOTONIC, &t1);

		pthread_mutex_lock(&x->lock);
		struct snap *cur = x->cur; /* old, or the last checkpoint */
		x->cur = sc.new;
		x->dirs_read = sc.read;
		x->scans++;
		pthread_mutex_unlock(&x->lock);
		x->h->log(x->h, MM_LOG_INFO, "music index: %ld files in %zu folders, %ld read, in %ld ms",
		          sc.new->files, sc.new->n, sc.read,
		          (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
		if (sc.read || sc.shown || !sc.old || sc.old->n != sc.new->n) save(x, sc.new);
		if (cur != sc.old) snap_free(cur);
		snap_free((struct snap *)sc.old);
	}
}

/* ------------------------------------------------------------------ api */

struct ffidx *ffidx_open(const struct mm_host *h, const char *root, const char *saved)
{
	struct ffidx *x = calloc(1, sizeof(*x));
	if (!x) return NULL;
	x->h = h;
	snprintf(x->root, sizeof(x->root), "%s", root);
	size_t n = strlen(x->root);
	while (n > 1 && x->root[n - 1] == '/') x->root[--n] = '\0';
	snprintf(x->saved, sizeof(x->saved), "%s", saved);
	pthread_mutex_init(&x->lock, NULL);
	x->checkpoint_s = CHECKPOINT_S;
	x->cur = load(x->saved, x->root);
	ffidx_rescan(x);
	return x;
}

void ffidx_checkpoint_every(struct ffidx *x, int seconds)
{
	pthread_mutex_lock(&x->lock);
	x->checkpoint_s = seconds < 0 ? 0 : seconds;
	pthread_mutex_unlock(&x->lock);
}

void ffidx_rescan(struct ffidx *x)
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

/* Lower-case ASCII; other bytes (UTF-8) compare as they are. */
static void lower(const char *in, char *out, size_t cap)
{
	size_t i = 0;
	for (; in[i] && i + 1 < cap; i++) out[i] = (in[i] >= 'A' && in[i] <= 'Z') ? (char)(in[i] + 32) : in[i];
	out[i] = '\0';
}

void ffidx_search(struct ffidx *x, const char *words, mm_item_fn emit, void *ctx)
{
	char low[1024], w[16][128], rel[8192], path[4096 + 8192 + 2], *save = NULL;
	int nw = 0;
	lower(words, low, sizeof(low));
	for (char *t = strtok_r(low, " \t", &save); t && nw < 16; t = strtok_r(NULL, " \t", &save))
		snprintf(w[nw++], sizeof(w[0]), "%s", t);
	if (!nw) return;

	pthread_mutex_lock(&x->lock);
	const struct snap *s = x->cur;
	int stop = 0;
	/* Folders are sorted by path, files within one in natural order. */
	for (size_t i = 0; s && i < s->n && !stop; i++) {
		const struct dir *d = &s->v[i];
		for (size_t k = 0; k < d->nfiles && !stop; k++) {
			snprintf(rel, sizeof(rel), "%s%s%s", d->rel, d->rel[0] ? "/" : "", d->files[k]);
			char lrel[8192];
			lower(rel, lrel, sizeof(lrel));
			int all = 1;
			for (int j = 0; j < nw && all; j++) all = strstr(lrel, w[j]) != NULL;
			if (!all) continue;
			snprintf(path, sizeof(path), "%s/%s", x->root, rel);
			struct mm_item it = { MM_ITEM_TRACK, path, d->files[k], NULL, NULL, 0 };
			stop = emit(ctx, &it) != 0;
		}
	}
	pthread_mutex_unlock(&x->lock);
}

void ffidx_stats(struct ffidx *x, struct ffidx_stats *out)
{
	pthread_mutex_lock(&x->lock);
	memset(out, 0, sizeof(*out));
	if (x->cur) {
		out->dirs = (long)x->cur->n;
		out->files = x->cur->files;
	}
	out->dirs_read = x->dirs_read;
	out->scanning = x->running;
	out->scans = x->scans;
	pthread_mutex_unlock(&x->lock);
}

void ffidx_close(struct ffidx *x)
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

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file mpris.c
 * @brief MPRIS 2 on the session bus, so playerctl, desktop media keys and
 * widgets can drive mediamaster.
 *
 * Implements org.mpris.MediaPlayer2 and org.mpris.MediaPlayer2.Player
 * (no TrackList, no Playlists) at /org/mpris/MediaPlayer2, under the
 * bus name org.mpris.MediaPlayer2.<mpris.name> (default "mediamaster";
 * ".instance<pid>" is appended if the name is taken).
 *
 * Everything goes through the host like an mmctl client would: methods
 * become commands (play, pause, next, seek, vol, ...), properties are
 * read from `status` and `queue`. Host events arrive on the main thread
 * and are handed to the bus thread through an eventfd; the bus thread
 * emits PropertiesChanged and Seeked.
 *
 * sd-bus objects are not thread-safe, so the bus thread owns the bus.
 *
 * Config:
 *   mpris.name = mediamaster */

#include "mm_plugin.h"
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#ifdef MM_HAVE_BASU
#include <basu/sd-bus.h>
#else
#include <systemd/sd-bus.h>
#endif

#define OBJ "/org/mpris/MediaPlayer2"
#define IF_ROOT "org.mpris.MediaPlayer2"
#define IF_PLAYER "org.mpris.MediaPlayer2.Player"
#define TRACK_PREFIX "/org/mediamaster/track/"
#define NO_TRACK "/org/mpris/MediaPlayer2/TrackList/NoTrack"

struct snap {
	char state[16];   /* stopped | playing | paused */
	unsigned item;
	char uri[1024];
	double position;  /* seconds */
	double duration;  /* seconds, 0 unknown */
	int seekable;
	int volume;       /* 0..100 */
	long qlen, qcur;  /* queue length, cursor (1-based, 0 none) */
	/* Tags, "" when the source gave none. Lists are newline-separated. */
	char title[512], artists[1024], album[512], albumartists[1024], art[1024];
	int track;
};

struct mpris {
	const struct mm_host *h;
	sd_bus *bus;
	pthread_t thread;
	int running;
	int efd;
	char busname[256];

	pthread_mutex_t lock; /* pending, quit */
	uint32_t pending;
	int quit;
};

/* ------------------------------------------------------------------ host access (bus thread) */

static int cmd(struct mpris *m, char *reply, size_t cap, const char *a0, const char *a1, const char *a2)
{
	const char *argv[4] = { a0, a1, a2, NULL };
	int argc = a2 ? 3 : a1 ? 2 : 1;
	return m->h->command(m->h, argc, argv, reply, cap);
}

static const char *field(const char *text, const char *key, char *out, size_t cap)
{
	size_t kl = strlen(key);
	for (const char *p = text; p && *p; ) {
		if (!strncmp(p, key, kl) && p[kl] == ':' && p[kl + 1] == ' ') {
			const char *v = p + kl + 2, *e = strchr(v, '\n');
			size_t n = e ? (size_t)(e - v) : strlen(v);
			if (n >= cap) n = cap - 1;
			memcpy(out, v, n);
			out[n] = '\0';
			return out;
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	out[0] = '\0';
	return NULL;
}

/* Every value of `key`, newline-separated, into out. */
static void fields(const char *text, const char *key, char *out, size_t cap)
{
	size_t kl = strlen(key), o = 0;
	out[0] = '\0';
	for (const char *p = text; p && *p; ) {
		if (!strncmp(p, key, kl) && p[kl] == ':' && p[kl + 1] == ' ') {
			const char *v = p + kl + 2, *e = strchr(v, '\n');
			size_t n = e ? (size_t)(e - v) : strlen(v);
			if (o + (o > 0) + n + 1 > cap) break;
			if (o) out[o++] = '\n';
			memcpy(out + o, v, n);
			o += n;
			out[o] = '\0';
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
}

static void take_snapshot(struct mpris *m, struct snap *s)
{
	static char buf[16384]; /* bus thread only */
	char v[1024];
	memset(s, 0, sizeof(*s));
	snprintf(s->state, sizeof(s->state), "stopped");
	if (cmd(m, buf, sizeof(buf), "status", NULL, NULL) == MM_REPLY_RAW) {
		if (field(buf, "state", v, sizeof(v))) snprintf(s->state, sizeof(s->state), "%.15s", v);
		if (field(buf, "item", v, sizeof(v))) s->item = (unsigned)strtoul(v, NULL, 10);
		if (field(buf, "uri", v, sizeof(v)) && strcmp(v, "-")) snprintf(s->uri, sizeof(s->uri), "%s", v);
		if (field(buf, "position", v, sizeof(v))) s->position = strtod(v, NULL);
		if (field(buf, "duration", v, sizeof(v)) && strcmp(v, "-")) s->duration = strtod(v, NULL);
		if (field(buf, "seekable", v, sizeof(v))) s->seekable = !strcmp(v, "yes");
		if (field(buf, "volume", v, sizeof(v))) s->volume = atoi(v);
		field(buf, "title", s->title, sizeof(s->title));
		fields(buf, "artist", s->artists, sizeof(s->artists));
		field(buf, "album", s->album, sizeof(s->album));
		fields(buf, "albumartist", s->albumartists, sizeof(s->albumartists));
		field(buf, "art", s->art, sizeof(s->art));
		if (field(buf, "tracknumber", v, sizeof(v))) s->track = atoi(v);
	}
	if (cmd(m, buf, sizeof(buf), "queue", "list", NULL) == MM_REPLY_RAW && strncmp(buf, "queue is empty", 14)) {
		long n = 0;
		for (const char *p = buf; *p; ) {
			n++;
			if (p[0] == '>') s->qcur = n;
			p = strchr(p, '\n');
			if (!p) break;
			p++;
		}
		s->qlen = n;
	}
}

static int can_next(const struct snap *s) { return s->qlen > 0 && s->qcur < s->qlen; }
static int can_prev(const struct snap *s) { return s->qcur > 1; }
static int can_play(const struct snap *s) { return s->qlen > 0 || strcmp(s->state, "stopped"); }
static int can_pause(const struct snap *s) { return strcmp(s->state, "stopped") != 0; }

static const char *playback_status(const struct snap *s)
{
	if (!strcmp(s->state, "playing")) return "Playing";
	if (!strcmp(s->state, "paused")) return "Paused";
	return "Stopped";
}

static void track_path(const struct snap *s, char *out, size_t cap)
{
	if (s->item) snprintf(out, cap, TRACK_PREFIX "%u", s->item);
	else snprintf(out, cap, NO_TRACK);
}

/* A title until real tags exist: the last path segment, %XX decoded. */
static void title_of(const char *uri, char *out, size_t cap)
{
	const char *t = uri;
	const char *q = strpbrk(uri, "?#");
	size_t n = q ? (size_t)(q - uri) : strlen(uri);
	for (size_t i = 0; i < n; i++)
		if (uri[i] == '/') t = uri + i + 1;
	size_t o = 0;
	for (const char *p = t; p < uri + n && o + 1 < cap; p++) {
		if (p[0] == '%' && p + 2 < uri + n + 1 && p[1] && p[2]) {
			char hex[3] = { p[1], p[2], 0 };
			char *e;
			long c = strtol(hex, &e, 16);
			if (!*e && c > 0) { out[o++] = (char)c; p += 2; continue; }
		}
		out[o++] = *p;
	}
	out[o] = '\0';
	if (!o) snprintf(out, cap, "%s", uri);
}

/* ------------------------------------------------------------------ properties */

#define GETTER(name) static int name(sd_bus *bus, const char *path, const char *iface, \
	const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err)
#define UNUSED_GETTER_ARGS (void)bus; (void)path; (void)iface; (void)prop; (void)err

GETTER(get_true) { UNUSED_GETTER_ARGS; (void)ud; return sd_bus_message_append(reply, "b", 1); }
GETTER(get_false) { UNUSED_GETTER_ARGS; (void)ud; return sd_bus_message_append(reply, "b", 0); }
GETTER(get_identity) { UNUSED_GETTER_ARGS; (void)ud; return sd_bus_message_append(reply, "s", "mediamaster"); }
GETTER(get_rate) { UNUSED_GETTER_ARGS; (void)ud; return sd_bus_message_append(reply, "d", 1.0); }

GETTER(get_schemes)
{
	UNUSED_GETTER_ARGS; (void)ud;
	return sd_bus_message_append(reply, "as", 3, "file", "http", "https");
}

GETTER(get_mimes)
{
	UNUSED_GETTER_ARGS; (void)ud;
	return sd_bus_message_append(reply, "as", 8, "audio/mpeg", "audio/flac", "audio/ogg", "audio/opus",
	                             "audio/x-wav", "audio/aac", "audio/mp4", "audio/webm");
}

GETTER(get_status)
{
	UNUSED_GETTER_ARGS;
	struct snap s;
	take_snapshot(ud, &s);
	return sd_bus_message_append(reply, "s", playback_status(&s));
}

GETTER(get_volume)
{
	UNUSED_GETTER_ARGS;
	struct snap s;
	take_snapshot(ud, &s);
	return sd_bus_message_append(reply, "d", s.volume / 100.0);
}

GETTER(get_position)
{
	UNUSED_GETTER_ARGS;
	struct snap s;
	take_snapshot(ud, &s);
	return sd_bus_message_append(reply, "x", (int64_t)(s.position * 1e6));
}

#define BOOL_GETTER(name, expr) GETTER(name) { UNUSED_GETTER_ARGS; struct snap s; take_snapshot(ud, &s); \
	return sd_bus_message_append(reply, "b", (int)(expr)); }
BOOL_GETTER(get_can_next, can_next(&s))
BOOL_GETTER(get_can_prev, can_prev(&s))
BOOL_GETTER(get_can_play, can_play(&s))
BOOL_GETTER(get_can_pause, can_pause(&s))
BOOL_GETTER(get_can_seek, s.seekable && s.item)

static int dict_open(sd_bus_message *r, const char *key, const char *sig)
{
	int rc = sd_bus_message_open_container(r, 'e', "sv");
	if (rc >= 0) rc = sd_bus_message_append(r, "s", key);
	if (rc >= 0) rc = sd_bus_message_open_container(r, 'v', sig);
	return rc;
}

static int dict_close(sd_bus_message *r)
{
	int rc = sd_bus_message_close_container(r);
	if (rc >= 0) rc = sd_bus_message_close_container(r);
	return rc;
}

static int dict_str(sd_bus_message *r, const char *key, const char *val)
{
	int rc;
	if (!val[0]) return 0;
	if ((rc = dict_open(r, key, "s")) < 0 || (rc = sd_bus_message_append(r, "s", val)) < 0) return rc;
	return dict_close(r);
}

/* A newline-separated list as an "as" entry. */
static int dict_strv(sd_bus_message *r, const char *key, const char *list)
{
	char one[1024];
	int rc;
	if (!list[0]) return 0;
	if ((rc = dict_open(r, key, "as")) < 0 || (rc = sd_bus_message_open_container(r, 'a', "s")) < 0)
		return rc;
	for (const char *p = list; p; ) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n >= sizeof(one)) n = sizeof(one) - 1;
		memcpy(one, p, n);
		one[n] = '\0';
		if ((rc = sd_bus_message_append(r, "s", one)) < 0) return rc;
		p = e ? e + 1 : NULL;
	}
	if ((rc = sd_bus_message_close_container(r)) < 0) return rc;
	return dict_close(r);
}

GETTER(get_metadata)
{
	UNUSED_GETTER_ARGS;
	struct snap s;
	char tp[96], title[1024];
	take_snapshot(ud, &s);
	track_path(&s, tp, sizeof(tp));
	int rc = sd_bus_message_open_container(reply, 'a', "{sv}");
	if (rc < 0) return rc;
	if ((rc = dict_open(reply, "mpris:trackid", "o")) < 0 ||
	    (rc = sd_bus_message_append(reply, "o", tp)) < 0 || (rc = dict_close(reply)) < 0)
		return rc;
	if (s.item && s.uri[0]) {
		if (s.title[0]) snprintf(title, sizeof(title), "%s", s.title);
		else title_of(s.uri, title, sizeof(title));
		if ((rc = dict_str(reply, "xesam:url", s.uri)) < 0 ||
		    (rc = dict_str(reply, "xesam:title", title)) < 0 ||
		    (rc = dict_strv(reply, "xesam:artist", s.artists)) < 0 ||
		    (rc = dict_str(reply, "xesam:album", s.album)) < 0 ||
		    (rc = dict_strv(reply, "xesam:albumArtist", s.albumartists)) < 0 ||
		    (rc = dict_str(reply, "mpris:artUrl", s.art)) < 0)
			return rc;
		if (s.track > 0) {
			if ((rc = dict_open(reply, "xesam:trackNumber", "i")) < 0 ||
			    (rc = sd_bus_message_append(reply, "i", (int32_t)s.track)) < 0 ||
			    (rc = dict_close(reply)) < 0)
				return rc;
		}
		if (s.duration > 0) {
			if ((rc = dict_open(reply, "mpris:length", "x")) < 0 ||
			    (rc = sd_bus_message_append(reply, "x", (int64_t)(s.duration * 1e6))) < 0 ||
			    (rc = dict_close(reply)) < 0)
				return rc;
		}
	}
	return sd_bus_message_close_container(reply);
}

static int set_volume(sd_bus *bus, const char *path, const char *iface, const char *prop,
                      sd_bus_message *value, void *ud, sd_bus_error *err)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)err;
	double v;
	int rc = sd_bus_message_read(value, "d", &v);
	if (rc < 0) return rc;
	if (v < 0) v = 0;
	if (v > 1) v = 1;
	char pct[16], r[64];
	snprintf(pct, sizeof(pct), "%d", (int)(v * 100.0 + 0.5));
	cmd(ud, r, sizeof(r), "vol", pct, NULL);
	return 0;
}

/* Clients may set Rate; only 1.0 is supported, anything else is ignored. */
static int set_ignore(sd_bus *bus, const char *path, const char *iface, const char *prop,
                      sd_bus_message *value, void *ud, sd_bus_error *err)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
	return sd_bus_message_skip(value, NULL);
}

/* ------------------------------------------------------------------ methods */

#define METHOD(name) static int name(sd_bus_message *msg, void *ud, sd_bus_error *err)

static int simple(sd_bus_message *msg, struct mpris *m, const char *a0, const char *a1)
{
	char r[512];
	cmd(m, r, sizeof(r), a0, a1, NULL); /* MPRIS: a method that cannot apply does nothing */
	return sd_bus_reply_method_return(msg, "");
}

METHOD(m_raise) { (void)ud; (void)err; return sd_bus_reply_method_return(msg, ""); }
METHOD(m_quit) { (void)err; return simple(msg, ud, "quit", NULL); }
METHOD(m_next) { (void)err; return simple(msg, ud, "next", NULL); }
METHOD(m_prev) { (void)err; return simple(msg, ud, "prev", NULL); }
METHOD(m_pause) { (void)err; return simple(msg, ud, "pause", NULL); }
METHOD(m_stop) { (void)err; return simple(msg, ud, "stop", NULL); }
METHOD(m_play) { (void)err; return simple(msg, ud, "play", NULL); }

METHOD(m_playpause)
{
	(void)err;
	struct snap s;
	take_snapshot(ud, &s);
	return simple(msg, ud, !strcmp(s.state, "stopped") ? "play" : "toggle", NULL);
}

static void seconds(char *out, size_t cap, int64_t us, int with_sign)
{
	int64_t a = us < 0 ? -us : us;
	snprintf(out, cap, "%s%" PRId64 ".%06" PRId64, with_sign ? (us < 0 ? "-" : "+") : "", a / 1000000, a % 1000000);
}

METHOD(m_seek)
{
	(void)err;
	int64_t off;
	int rc = sd_bus_message_read(msg, "x", &off);
	if (rc < 0) return rc;
	char t[48];
	seconds(t, sizeof(t), off, 1);
	return simple(msg, ud, "seek", t);
}

METHOD(m_setpos)
{
	(void)err;
	const char *track;
	int64_t pos;
	int rc = sd_bus_message_read(msg, "ox", &track, &pos);
	if (rc < 0) return rc;
	struct snap s;
	char tp[96], t[48];
	take_snapshot(ud, &s);
	track_path(&s, tp, sizeof(tp));
	/* Spec: ignore a stale track id or a position outside the track. */
	if (strcmp(track, tp) || pos < 0 || (s.duration > 0 && (double)pos > s.duration * 1e6))
		return sd_bus_reply_method_return(msg, "");
	seconds(t, sizeof(t), pos, 0);
	return simple(msg, ud, "seek", t);
}

METHOD(m_openuri)
{
	const char *uri;
	int rc = sd_bus_message_read(msg, "s", &uri);
	if (rc < 0) return rc;
	char r[512];
	if (cmd(ud, r, sizeof(r), "play", uri, NULL) != MM_OK)
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", r);
	return sd_bus_reply_method_return(msg, "");
}

#define EMITS SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE
#define CONST SD_BUS_VTABLE_PROPERTY_CONST

static const sd_bus_vtable root_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Raise", "", "", m_raise, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Quit", "", "", m_quit, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("CanQuit", "b", get_true, 0, CONST),
	SD_BUS_PROPERTY("CanRaise", "b", get_false, 0, CONST),
	SD_BUS_PROPERTY("HasTrackList", "b", get_false, 0, CONST),
	SD_BUS_PROPERTY("Identity", "s", get_identity, 0, CONST),
	SD_BUS_PROPERTY("SupportedUriSchemes", "as", get_schemes, 0, CONST),
	SD_BUS_PROPERTY("SupportedMimeTypes", "as", get_mimes, 0, CONST),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable player_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Next", "", "", m_next, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Previous", "", "", m_prev, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Pause", "", "", m_pause, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("PlayPause", "", "", m_playpause, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Stop", "", "", m_stop, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Play", "", "", m_play, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Seek", "x", "", m_seek, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SetPosition", "ox", "", m_setpos, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("OpenUri", "s", "", m_openuri, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("Seeked", "x", 0),
	SD_BUS_PROPERTY("PlaybackStatus", "s", get_status, 0, EMITS),
	SD_BUS_WRITABLE_PROPERTY("Rate", "d", get_rate, set_ignore, 0, EMITS),
	SD_BUS_PROPERTY("Metadata", "a{sv}", get_metadata, 0, EMITS),
	SD_BUS_WRITABLE_PROPERTY("Volume", "d", get_volume, set_volume, 0, EMITS),
	SD_BUS_PROPERTY("Position", "x", get_position, 0, 0), /* spec: no change signal */
	SD_BUS_PROPERTY("MinimumRate", "d", get_rate, 0, CONST),
	SD_BUS_PROPERTY("MaximumRate", "d", get_rate, 0, CONST),
	SD_BUS_PROPERTY("CanGoNext", "b", get_can_next, 0, EMITS),
	SD_BUS_PROPERTY("CanGoPrevious", "b", get_can_prev, 0, EMITS),
	SD_BUS_PROPERTY("CanPlay", "b", get_can_play, 0, EMITS),
	SD_BUS_PROPERTY("CanPause", "b", get_can_pause, 0, EMITS),
	SD_BUS_PROPERTY("CanSeek", "b", get_can_seek, 0, EMITS),
	SD_BUS_PROPERTY("CanControl", "b", get_true, 0, CONST),
	SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------------ bus thread */

static void emit(struct mpris *m, uint32_t ev)
{
	if (ev & (MM_EV_STATE | MM_EV_TRACK | MM_EV_QUEUE | MM_EV_META))
		sd_bus_emit_properties_changed(m->bus, OBJ, IF_PLAYER, "PlaybackStatus", "Metadata",
		                               "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek", NULL);
	if (ev & MM_EV_VOLUME)
		sd_bus_emit_properties_changed(m->bus, OBJ, IF_PLAYER, "Volume", NULL);
	if (ev & MM_EV_SEEK) {
		/* Spec: Seeked when the position jumps (a new track implies 0). */
		struct snap s;
		take_snapshot(m, &s);
		sd_bus_emit_signal(m->bus, OBJ, IF_PLAYER, "Seeked", "x", (int64_t)(s.position * 1e6));
	}
}

static int bus_timeout_ms(sd_bus *bus)
{
	uint64_t at;
	if (sd_bus_get_timeout(bus, &at) < 0 || at == UINT64_MAX) return -1;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
	return at <= now ? 0 : (int)((at - now + 999) / 1000);
}

static void *bus_thread(void *arg)
{
	struct mpris *m = arg;
	for (;;) {
		int r;
		while ((r = sd_bus_process(m->bus, NULL)) > 0) { }
		if (r < 0) {
			m->h->log(m->h, MM_LOG_WARN, "bus: %s", strerror(-r));
			break;
		}
		struct pollfd pf[2] = {
			{ sd_bus_get_fd(m->bus), (short)sd_bus_get_events(m->bus), 0 },
			{ m->efd, POLLIN, 0 },
		};
		if (poll(pf, 2, bus_timeout_ms(m->bus)) < 0 && errno != EINTR) break;
		if (pf[1].revents & POLLIN) {
			uint64_t v;
			if (read(m->efd, &v, sizeof(v)) < 0 && errno != EAGAIN) break;
			pthread_mutex_lock(&m->lock);
			uint32_t ev = m->pending;
			int quit = m->quit;
			m->pending = 0;
			pthread_mutex_unlock(&m->lock);
			if (quit) break;
			if (ev) emit(m, ev);
		}
	}
	return NULL;
}

/* Main thread: hand the events to the bus thread. */
static void on_event(void *priv, uint32_t ev)
{
	struct mpris *m = priv;
	uint64_t one = 1;
	pthread_mutex_lock(&m->lock);
	m->pending |= ev;
	pthread_mutex_unlock(&m->lock);
	if (write(m->efd, &one, sizeof(one)) < 0) { /* counter saturated: already pending */ }
}

/* ------------------------------------------------------------------ lifecycle */

static int acquire_name(struct mpris *m)
{
	const char *n = m->h->config_get(m->h, "name");
	if (!n || !*n) n = "mediamaster";
	snprintf(m->busname, sizeof(m->busname), "org.mpris.MediaPlayer2.%s", n);
	int rc = sd_bus_request_name(m->bus, m->busname, 0);
	if (rc == -EEXIST) {
		snprintf(m->busname, sizeof(m->busname), "org.mpris.MediaPlayer2.%s.instance%ld", n, (long)getpid());
		rc = sd_bus_request_name(m->bus, m->busname, 0);
	}
	return rc;
}

static void teardown(struct mpris *m)
{
	if (m->running) {
		uint64_t one = 1;
		pthread_mutex_lock(&m->lock);
		m->quit = 1;
		pthread_mutex_unlock(&m->lock);
		if (write(m->efd, &one, sizeof(one)) < 0) { /* pending already */ }
		pthread_join(m->thread, NULL);
		m->running = 0;
	}
	if (m->bus) {
		sd_bus_flush_close_unref(m->bus);
		m->bus = NULL;
	}
	if (m->efd >= 0) close(m->efd);
	pthread_mutex_destroy(&m->lock);
	free(m);
}

static int mpris_init(const struct mm_host *h, void **priv)
{
	if (h->size < sizeof(struct mm_host) || !h->command || !h->subscribe) {
		h->log(h, MM_LOG_WARN, "host too old for mpris");
		return -1;
	}
	struct mpris *m = calloc(1, sizeof(*m));
	if (!m) return -1;
	m->h = h;
	m->efd = -1;
	pthread_mutex_init(&m->lock, NULL);

	int rc = sd_bus_open_user(&m->bus);
	if (rc < 0) {
		/* Normal on a headless machine: stay loaded, do nothing. */
		h->log(h, MM_LOG_INFO, "no session bus (%s); MPRIS off", strerror(-rc));
		m->bus = NULL;
		*priv = m;
		return 0;
	}
	if ((rc = sd_bus_add_object_vtable(m->bus, NULL, OBJ, IF_ROOT, root_vtable, m)) < 0 ||
	    (rc = sd_bus_add_object_vtable(m->bus, NULL, OBJ, IF_PLAYER, player_vtable, m)) < 0) {
		h->log(h, MM_LOG_WARN, "vtable: %s", strerror(-rc));
		goto fail;
	}
	if ((rc = acquire_name(m)) < 0) {
		h->log(h, MM_LOG_WARN, "bus name: %s", strerror(-rc));
		goto fail;
	}
	m->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (m->efd < 0) goto fail;
	if (h->subscribe(h, MM_EV_STATE | MM_EV_TRACK | MM_EV_VOLUME | MM_EV_SEEK | MM_EV_QUEUE | MM_EV_META,
	                 on_event, m) != 0)
		goto fail;
	if (pthread_create(&m->thread, NULL, bus_thread, m) != 0) goto fail;
	m->running = 1;
	h->log(h, MM_LOG_INFO, "on the session bus as %s", m->busname);
	*priv = m;
	return 0;

fail:
	teardown(m);
	return -1;
}

static void mpris_fini(void *priv)
{
	teardown(priv);
}

static const struct mm_plugin s_plugin = {
	.abi = MM_ABI,
	.name = "mpris",
	.version = "0.1.0",
	.description = "MPRIS 2 on the session bus (playerctl, media keys)",
	.init = mpris_init,
	.fini = mpris_fini,
};

MM_PLUGIN_DEFINE(mpris)
{
	return &s_plugin;
}

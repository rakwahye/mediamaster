/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file pipewire.c
 * @brief PipeWire playback sink.
 *
 * One output stream, F32 48 kHz stereo, realtime process callback that
 * calls the host's pull directly into the PipeWire buffer.
 *
 * Config:
 *   pipewire.target     = <node.name>   pin to a device; empty = default
 *   pipewire.latency_ms = 20            requested quantum
 *   pipewire.timeout_ms = 2000          how long start waits to connect */

#include "mm_plugin.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pw_priv {
	const struct mm_host *h;
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	struct spa_hook listener;
	mm_pull_fn pull;
	void *ctx;
	enum pw_stream_state state;
	int got_state;
};

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	struct pw_priv *s = data;
	(void)old;
	s->state = state;
	s->got_state = 1;
	if (state == PW_STREAM_STATE_ERROR)
		s->h->log(s->h, MM_LOG_WARN, "stream error: %s", error ? error : "?");
	else
		s->h->log(s->h, MM_LOG_DEBUG, "stream %s", pw_stream_state_as_string(state));
	pw_thread_loop_signal(s->loop, 0);
}

static void on_process(void *data)
{
	struct pw_priv *s = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(s->stream);
	if (!b) return;
	struct spa_data *d = &b->buffer->datas[0];
	if (!d->data) {
		pw_stream_queue_buffer(s->stream, b);
		return;
	}
	const uint32_t stride = MM_PCM_CHANNELS * sizeof(float);
	uint32_t n = d->maxsize / stride;
	if (b->requested && b->requested < n) n = (uint32_t)b->requested;
	s->pull(s->ctx, d->data, n);
	d->chunk->offset = 0;
	d->chunk->stride = (int32_t)stride;
	d->chunk->size = n * stride;
	pw_stream_queue_buffer(s->stream, b);
}

static const struct pw_stream_events s_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state,
	.process = on_process,
};

static long cfg_long(const struct mm_host *h, const char *k, long def)
{
	const char *v = h->config_get(h, k);
	return (v && *v) ? atol(v) : def;
}

static void teardown(struct pw_priv *s)
{
	if (s->loop) pw_thread_loop_stop(s->loop);
	if (s->stream) { pw_stream_destroy(s->stream); s->stream = NULL; }
	if (s->loop) { pw_thread_loop_destroy(s->loop); s->loop = NULL; }
}

static int pwsink_start(void *priv, mm_pull_fn pull, void *ctx)
{
	struct pw_priv *s = priv;
	if (s->loop) return -1;
	s->pull = pull;
	s->ctx = ctx;
	s->got_state = 0;

	s->loop = pw_thread_loop_new("mediamaster-out", NULL);
	if (!s->loop) return -1;

	char latency[32];
	long lat = cfg_long(s->h, "latency_ms", 20);
	if (lat < 1) lat = 1;
	snprintf(latency, sizeof(latency), "%ld/%u", lat * (long)MM_PCM_RATE / 1000, MM_PCM_RATE);

	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_MEDIA_ROLE, "Music",
		PW_KEY_APP_NAME, "mediamaster",
		PW_KEY_NODE_NAME, "mediamaster",
		PW_KEY_NODE_LATENCY, latency,
		NULL);
	const char *target = s->h->config_get(s->h, "target");
	if (props && target && *target) pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);

	s->stream = pw_stream_new_simple(pw_thread_loop_get_loop(s->loop), "mediamaster", props, &s_events, s);
	if (!s->stream) {
		teardown(s);
		return -1;
	}

	uint8_t pod[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof(pod));
	struct spa_audio_info_raw raw;
	memset(&raw, 0, sizeof(raw));
	raw.format = SPA_AUDIO_FORMAT_F32;
	raw.rate = MM_PCM_RATE;
	raw.channels = MM_PCM_CHANNELS;
	raw.position[0] = SPA_AUDIO_CHANNEL_FL;
	raw.position[1] = SPA_AUDIO_CHANNEL_FR;
	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);

	int rc = pw_stream_connect(s->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
	                           PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
	                           PW_STREAM_FLAG_RT_PROCESS,
	                           params, 1);
	if (rc < 0) {
		s->h->log(s->h, MM_LOG_WARN, "connect: %s", spa_strerror(rc));
		teardown(s);
		return -1;
	}
	if (pw_thread_loop_start(s->loop) < 0) {
		teardown(s);
		return -1;
	}

	/* Wait until the stream settles: ready (paused/streaming) or failed.
	 * Without a daemon the core connection errors out quickly. */
	long timeout_ms = cfg_long(s->h, "timeout_ms", 2000);
	pw_thread_loop_lock(s->loop);
	while (s->state != PW_STREAM_STATE_PAUSED && s->state != PW_STREAM_STATE_STREAMING &&
	       s->state != PW_STREAM_STATE_ERROR && timeout_ms > 0) {
		if (pw_thread_loop_timed_wait(s->loop, 1) != 0) timeout_ms -= 1000;
		else if (s->state == PW_STREAM_STATE_UNCONNECTED && s->got_state) break;
	}
	enum pw_stream_state st = s->state;
	pw_thread_loop_unlock(s->loop);

	if (st != PW_STREAM_STATE_PAUSED && st != PW_STREAM_STATE_STREAMING) {
		s->h->log(s->h, MM_LOG_WARN, "no PipeWire output (%s)", pw_stream_state_as_string(st));
		teardown(s);
		return -1;
	}
	s->h->log(s->h, MM_LOG_INFO, "output ready, latency %s", latency);
	return 0;
}

static void pwsink_stop(void *priv)
{
	teardown(priv);
}

static int pwsink_init(const struct mm_host *h, void **priv)
{
	struct pw_priv *s = calloc(1, sizeof(*s));
	if (!s) return -1;
	s->h = h;
	pw_init(NULL, NULL);
	*priv = s;
	return 0;
}

static void pwsink_fini(void *priv)
{
	teardown(priv);
	free(priv);
	pw_deinit();
}

static const struct mm_sink_ops s_sink = {
	.start = pwsink_start,
	.stop = pwsink_stop,
};

static const struct mm_plugin s_plugin = {
	.abi = MM_ABI,
	.name = "pipewire",
	.version = "0.1.0",
	.description = "PipeWire playback",
	.init = pwsink_init,
	.fini = pwsink_fini,
	.sink = &s_sink,
};

MM_PLUGIN_DEFINE(pipewire)
{
	return &s_plugin;
}

/*
 * output_pipewire.c — Native PipeWire backend (nightly track).
 *
 * Uses pw_thread_loop to own a dedicated realtime-ish thread for the
 * stream's process callback, with a small internal ring buffer that
 * spotifygtk_output_write() fills and the process callback drains.
 * This is real PipeWire API usage (not protocol guesswork like the
 * Spotify AP layer), but the SPA POD format-negotiation surface is
 * fiddly — worth validating against a running PipeWire instance
 * before relying on it, since small mistakes there tend to fail as
 * "stream never activates" rather than a compile error.
 */

#include "config.h"
#include "output.h"

#if HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <string.h>

#define PW_RING_FRAMES (1 << 16)  /* ~1.5s at 44.1kHz stereo */

typedef struct {
  struct pw_thread_loop *loop;
  struct pw_stream      *stream;

  gint16  *ring;
  gsize    ring_head, ring_tail, ring_fill;
  gint     channels;
} PipewireData;

static void
on_process (void *userdata)
{
  PipewireData *data = userdata;
  struct pw_buffer *b = pw_stream_dequeue_buffer (data->stream);
  if (!b) return;

  struct spa_buffer *buf = b->buffer;
  gint16 *dst = buf->datas[0].data;
  if (!dst) { pw_stream_queue_buffer (data->stream, b); return; }

  guint stride = (guint) (sizeof (gint16) * (gsize) data->channels);
  guint n_frames_wanted = buf->datas[0].maxsize / stride;

  gsize available = data->ring_fill;
  gsize to_copy = MIN ((gsize) n_frames_wanted, available);

  for (gsize i = 0; i < to_copy * (gsize) data->channels; i++) {
    dst[i] = data->ring[data->ring_head];
    data->ring_head = (data->ring_head + 1) % (PW_RING_FRAMES * (gsize) data->channels);
  }
  data->ring_fill -= to_copy;

  /* Underrun: pad with silence rather than glitching */
  for (gsize i = to_copy * (gsize) data->channels; i < (gsize) n_frames_wanted * (gsize) data->channels; i++)
    dst[i] = 0;

  buf->datas[0].chunk->offset = 0;
  buf->datas[0].chunk->stride = stride;
  buf->datas[0].chunk->size   = n_frames_wanted * stride;

  pw_stream_queue_buffer (data->stream, b);
}

static const struct pw_stream_events STREAM_EVENTS = {
  PW_VERSION_STREAM_EVENTS,
  .process = on_process,
};

static gsize
pipewire_write (SpotifyAudioOutput *self, const gint16 *samples, gsize n_frames)
{
  PipewireData *data = self->backend_data;
  pw_thread_loop_lock (data->loop);

  gsize cap = PW_RING_FRAMES;
  gsize space = cap - data->ring_fill;
  gsize take = MIN (n_frames, space);

  for (gsize i = 0; i < take * (gsize) data->channels; i++) {
    data->ring[data->ring_tail] = samples[i];
    data->ring_tail = (data->ring_tail + 1) % (cap * (gsize) data->channels);
  }
  data->ring_fill += take;

  pw_thread_loop_unlock (data->loop);
  return take;
}

static void
pipewire_drain (SpotifyAudioOutput *self)
{
  PipewireData *data = self->backend_data;
  /* Busy-wait for the ring to empty; process callback runs on the
   * thread loop, so just poll with a short sleep. */
  while (TRUE) {
    pw_thread_loop_lock (data->loop);
    gboolean empty = (data->ring_fill == 0);
    pw_thread_loop_unlock (data->loop);
    if (empty) break;
    g_usleep (5000);
  }
}

static void
pipewire_close (SpotifyAudioOutput *self)
{
  PipewireData *data = self->backend_data;
  if (data->loop) {
    pw_thread_loop_stop (data->loop);
    if (data->stream) pw_stream_destroy (data->stream);
    pw_thread_loop_destroy (data->loop);
  }
  g_free (data->ring);
  g_free (data);
}

static const AudioBackendVtable PIPEWIRE_VTABLE = {
  .write      = pipewire_write,
  .set_volume = NULL,  /* per-stream volume via pw_stream_set_control(), TODO */
  .drain      = pipewire_drain,
  .close      = pipewire_close,
};

gboolean
output_pipewire_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{
  static gboolean pw_initialized = FALSE;
  if (!pw_initialized) {
    pw_init (NULL, NULL);
    pw_initialized = TRUE;
  }

  PipewireData *data = g_new0 (PipewireData, 1);
  data->channels = channels;
  data->ring = g_new0 (gint16, (gsize) PW_RING_FRAMES * (gsize) channels);

  data->loop = pw_thread_loop_new ("spotifygtk-audio", NULL);
  if (!data->loop) { g_free (data->ring); g_free (data); return FALSE; }

  pw_thread_loop_lock (data->loop);

  struct pw_properties *props = pw_properties_new (
    PW_KEY_MEDIA_TYPE, "Audio",
    PW_KEY_MEDIA_CATEGORY, "Playback",
    PW_KEY_MEDIA_ROLE, "Music",
    NULL);

  data->stream = pw_stream_new_simple (
    pw_thread_loop_get_loop (data->loop),
    "SpotifyGTK", props, &STREAM_EVENTS, data);

  guint8 buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
  struct spa_audio_info_raw info = {
    .format   = SPA_AUDIO_FORMAT_S16,
    .rate     = (guint32) rate,
    .channels = (guint32) channels,
  };
  const struct spa_pod *params[1];
  params[0] = spa_format_audio_raw_build (&b, SPA_PARAM_EnumFormat, &info);

  int res = pw_stream_connect (data->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
    PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
    params, 1);

  pw_thread_loop_unlock (data->loop);

  if (res < 0) {
    g_message ("PipeWire unavailable: connect failed (%s)", spa_strerror (res));
    pw_thread_loop_destroy (data->loop);
    g_free (data->ring);
    g_free (data);
    return FALSE;
  }

  pw_thread_loop_start (data->loop);

  self->vtable       = &PIPEWIRE_VTABLE;
  self->backend_data = data;
  return TRUE;
}

#else /* !HAVE_PIPEWIRE */

gboolean
output_pipewire_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{
  (void) self; (void) rate; (void) channels;
  return FALSE;
}

#endif

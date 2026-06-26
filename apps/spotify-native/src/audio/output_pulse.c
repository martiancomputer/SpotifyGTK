/*
 * output_pulse.c — PulseAudio backend (stable track default).
 *
 * Uses the "simple" blocking PulseAudio API — intentionally the
 * least sophisticated client API available, because it's also the
 * most stable: it's been essentially unchanged since PulseAudio 0.9
 * and is what most "just play some PCM" tools use under the hood.
 */

#include "config.h"
#include "output.h"

#if HAVE_PULSE
#include <pulse/simple.h>
#include <pulse/error.h>
#endif

typedef struct {
#if HAVE_PULSE
  pa_simple *stream;
#endif
  gint channels;
} PulseData;

static gsize
pulse_write (SpotifyAudioOutput *self, const gint16 *samples, gsize n_frames)
{
#if HAVE_PULSE
  PulseData *data = self->backend_data;
  int error = 0;
  gsize bytes = n_frames * (gsize) data->channels * sizeof (gint16);
  if (pa_simple_write (data->stream, samples, bytes, &error) < 0) {
    g_warning ("PulseAudio write failed: %s", pa_strerror (error));
    return 0;
  }
  return n_frames;
#else
  (void) self; (void) samples; (void) n_frames;
  return 0;
#endif
}

static void
pulse_drain (SpotifyAudioOutput *self)
{
#if HAVE_PULSE
  PulseData *data = self->backend_data;
  int error = 0;
  pa_simple_drain (data->stream, &error);
#else
  (void) self;
#endif
}

static void
pulse_close (SpotifyAudioOutput *self)
{
#if HAVE_PULSE
  PulseData *data = self->backend_data;
  if (data->stream) pa_simple_free (data->stream);
  g_free (data);
#else
  (void) self;
#endif
}

static const AudioBackendVtable PULSE_VTABLE = {
  .write      = pulse_write,
  .set_volume = NULL,   /* volume handled via PulseAudio's per-stream cork/volume API later */
  .drain      = pulse_drain,
  .close      = pulse_close,
};

gboolean
output_pulse_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{
#if HAVE_PULSE
  pa_sample_spec spec = {
    .format   = PA_SAMPLE_S16LE,
    .rate     = (guint32) rate,
    .channels = (guint8) channels,
  };

  int error = 0;
  pa_simple *stream = pa_simple_new (
    NULL, "SpotifyGTK", PA_STREAM_PLAYBACK, NULL,
    "Music playback", &spec, NULL, NULL, &error);

  if (!stream) {
    g_message ("PulseAudio unavailable: %s", pa_strerror (error));
    return FALSE;
  }

  PulseData *data = g_new0 (PulseData, 1);
  data->stream   = stream;
  data->channels = channels;

  self->vtable       = &PULSE_VTABLE;
  self->backend_data = data;
  return TRUE;
#else
  (void) self; (void) rate; (void) channels;
  return FALSE;
#endif
}

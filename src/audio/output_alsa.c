/*
 * output_alsa.c — ALSA backend (bare-metal fallback).
 *
 * Last resort when neither PipeWire nor PulseAudio sockets are
 * reachable — talks directly to the kernel's ALSA PCM interface.
 * This is the path that guarantees SpotifyGTK works even on a
 * minimal/embedded Linux install with no sound server running.
 */

#include "config.h"
#include "output.h"

#if HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

typedef struct {
#if HAVE_ALSA
  snd_pcm_t *handle;
#endif
  gint channels;
} AlsaData;

static gsize
alsa_write (SpotifyAudioOutput *self, const gint16 *samples, gsize n_frames)
{
#if HAVE_ALSA
  AlsaData *data = self->backend_data;
  snd_pcm_sframes_t written = snd_pcm_writei (data->handle, samples, n_frames);

  if (written < 0) {
    if (written == -EPIPE) {
      /* Buffer underrun — recover and retry once. */
      snd_pcm_prepare (data->handle);
      written = snd_pcm_writei (data->handle, samples, n_frames);
    }
    if (written < 0) {
      g_warning ("ALSA write failed: %s", snd_strerror ((int) written));
      return 0;
    }
  }
  return (gsize) written;
#else
  (void) self; (void) samples; (void) n_frames;
  return 0;
#endif
}

static void
alsa_drain (SpotifyAudioOutput *self)
{
#if HAVE_ALSA
  AlsaData *data = self->backend_data;
  snd_pcm_drain (data->handle);
#else
  (void) self;
#endif
}

static void
alsa_close (SpotifyAudioOutput *self)
{
#if HAVE_ALSA
  AlsaData *data = self->backend_data;
  if (data->handle) snd_pcm_close (data->handle);
  g_free (data);
#else
  (void) self;
#endif
}

static const AudioBackendVtable ALSA_VTABLE = {
  .write      = alsa_write,
  .set_volume = NULL,  /* ALSA mixer volume control is a separate, optional path */
  .drain      = alsa_drain,
  .close      = alsa_close,
};

gboolean
output_alsa_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{
#if HAVE_ALSA
  snd_pcm_t *handle = NULL;
  if (snd_pcm_open (&handle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
    g_message ("ALSA unavailable: could not open default device");
    return FALSE;
  }

  if (snd_pcm_set_params (handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                          (guint) channels, (guint) rate, 1 /* allow resample */,
                          100000 /* 100ms latency */) < 0) {
    g_message ("ALSA unavailable: set_params failed");
    snd_pcm_close (handle);
    return FALSE;
  }

  AlsaData *data = g_new0 (AlsaData, 1);
  data->handle   = handle;
  data->channels = channels;

  self->vtable       = &ALSA_VTABLE;
  self->backend_data = data;
  return TRUE;
#else
  (void) self; (void) rate; (void) channels;
  return FALSE;
#endif
}

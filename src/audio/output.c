/*
 * output.c — Backend probing and the public dispatch shim.
 *
 * This is the only file that decides backend priority order. Every
 * backend file is free-standing and conditionally compiled (see
 * audio/meson.build) — output.c just tries each candidate in order
 * and keeps the first one that successfully opens.
 */

#include "config.h"
#include "output.h"

static const AudioBackendKind PRIORITY_ORDER[] = {
#if HAVE_PIPEWIRE
  AUDIO_BACKEND_PIPEWIRE,
#endif
  AUDIO_BACKEND_PULSE,
  AUDIO_BACKEND_ALSA,
};

const gchar *
spotifygtk_output_backend_name (AudioBackendKind kind)
{
  switch (kind) {
    case AUDIO_BACKEND_PIPEWIRE: return "PipeWire";
    case AUDIO_BACKEND_PULSE:    return "PulseAudio";
    case AUDIO_BACKEND_ALSA:     return "ALSA";
    case AUDIO_BACKEND_WASAPI:   return "WASAPI";
    default:                     return "none";
  }
}

SpotifyAudioOutput *
spotifygtk_output_open (gint sample_rate, gint channels)
{
  SpotifyAudioOutput *out = g_new0 (SpotifyAudioOutput, 1);

  for (gsize i = 0; i < G_N_ELEMENTS (PRIORITY_ORDER); i++) {
    out->kind = PRIORITY_ORDER[i];
    gboolean ok = FALSE;

    switch (out->kind) {
#if HAVE_PIPEWIRE
      case AUDIO_BACKEND_PIPEWIRE: ok = output_pipewire_try_open (out, sample_rate, channels); break;
#endif
      case AUDIO_BACKEND_PULSE:    ok = output_pulse_try_open    (out, sample_rate, channels); break;
      case AUDIO_BACKEND_ALSA:     ok = output_alsa_try_open     (out, sample_rate, channels); break;
      default: break;
    }

    if (ok) {
      g_message ("Audio output: using %s", spotifygtk_output_backend_name (out->kind));
      return out;
    }
  }

  g_warning ("Audio output: no backend available (tried PipeWire/Pulse/ALSA as compiled in)");
  g_free (out);
  return NULL;
}

gsize
spotifygtk_output_write (SpotifyAudioOutput *self, const gint16 *samples, gsize n_frames)
{
  if (!self || !self->vtable || !self->vtable->write) return 0;
  return self->vtable->write (self, samples, n_frames);
}

void
spotifygtk_output_set_volume (SpotifyAudioOutput *self, gdouble volume_0_to_1)
{
  if (self && self->vtable && self->vtable->set_volume)
    self->vtable->set_volume (self, volume_0_to_1);
}

void
spotifygtk_output_drain (SpotifyAudioOutput *self)
{
  if (self && self->vtable && self->vtable->drain)
    self->vtable->drain (self);
}

void
spotifygtk_output_close (SpotifyAudioOutput *self)
{
  if (!self) return;
  if (self->vtable && self->vtable->close)
    self->vtable->close (self);
  g_free (self);
}

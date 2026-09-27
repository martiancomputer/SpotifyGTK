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
#ifdef G_OS_WIN32
  /* The POSIX backends are not built on Windows, so WASAPI is the whole list
   * rather than a fallback appended to it. */
  AUDIO_BACKEND_WASAPI,
#else
#if HAVE_PIPEWIRE
  AUDIO_BACKEND_PIPEWIRE,
#endif
  AUDIO_BACKEND_PULSE,
  AUDIO_BACKEND_ALSA,
#endif
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
spotifygtk_output_open (gint sample_rate, gint channels, gint format_bits)
{
  g_return_val_if_fail (format_bits == 16 || format_bits == 24 || format_bits == 32, NULL);
  SpotifyAudioOutput *out = g_new0 (SpotifyAudioOutput, 1);
  out->channels = channels;
  out->format_bits = format_bits;

  for (gsize i = 0; i < G_N_ELEMENTS (PRIORITY_ORDER); i++) {
    out->kind = PRIORITY_ORDER[i];
    gboolean ok = FALSE;

    switch (out->kind) {
#ifdef G_OS_WIN32
      case AUDIO_BACKEND_WASAPI:   ok = output_wasapi_try_open   (out, sample_rate, channels); break;
#else
#if HAVE_PIPEWIRE
      case AUDIO_BACKEND_PIPEWIRE: ok = output_pipewire_try_open (out, sample_rate, channels); break;
#endif
      case AUDIO_BACKEND_PULSE:    ok = output_pulse_try_open    (out, sample_rate, channels); break;
      case AUDIO_BACKEND_ALSA:     ok = output_alsa_try_open     (out, sample_rate, channels); break;
#endif
      default: break;
    }

    if (ok) {
      g_message ("Audio output: using %s", spotifygtk_output_backend_name (out->kind));
      return out;
    }
  }

  g_warning ("Audio output: no backend available (tried %s)",
#ifdef G_OS_WIN32
             "WASAPI"
#else
             "PipeWire/Pulse/ALSA as compiled in"
#endif
            );
  g_free (out);
  return NULL;
}

gsize
spotifygtk_output_write (SpotifyAudioOutput *self, const gint16 *samples, gsize n_frames)
{
  if (!self || !self->vtable || !self->vtable->write) return 0;
  if (self->format_bits == 16)
    return self->vtable->write (self, samples, n_frames);

  /* Spotify currently decodes to signed 16-bit PCM. Widen the container at
   * the output boundary; a future local-file decoder will also need an
   * upstream wide-PCM path, but backend negotiation is ready for one. */
  gsize count = n_frames * (gsize) self->channels;
  gsize width = (gsize) self->format_bits / 8;
  gsize bytes = count * width;
  if (bytes > self->pack_capacity) {
    self->pack_buffer = g_realloc (self->pack_buffer, bytes);
    self->pack_capacity = bytes;
  }
  guint8 *wide = self->pack_buffer;
  for (gsize i = 0; i < count; i++) {
    guint32 value = (guint32) ((gint32) samples[i] * 65536);
    if (width == 3) {
      wide[i * 3]     = (guint8) (value >> 8);
      wide[i * 3 + 1] = (guint8) (value >> 16);
      wide[i * 3 + 2] = (guint8) (value >> 24);
    } else {
      wide[i * 4]     = (guint8) value;
      wide[i * 4 + 1] = (guint8) (value >> 8);
      wide[i * 4 + 2] = (guint8) (value >> 16);
      wide[i * 4 + 3] = (guint8) (value >> 24);
    }
  }
  return self->vtable->write (self, wide, n_frames);
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
spotifygtk_output_flush (SpotifyAudioOutput *self)
{
  if (self && self->vtable && self->vtable->flush)
    self->vtable->flush (self);
}

void
spotifygtk_output_close (SpotifyAudioOutput *self)
{
  if (!self) return;
  if (self->vtable && self->vtable->close)
    self->vtable->close (self);
  g_free (self->pack_buffer);
  g_free (self);
}

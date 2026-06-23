/*
 * output.h — Abstract audio output interface.
 *
 * Every platform/backend (PulseAudio, ALSA, PipeWire, WASAPI) hides
 * behind this one vtable-style interface. player.c and decoder.c
 * never touch a backend directly — they call these functions, and
 * the backend is selected once at startup via runtime probing
 * (see spotifygtk_output_open()).
 *
 * This is the same "probe at runtime, not compile time" principle
 * used in image_cache.c — the binary works everywhere, it just picks
 * the best available path silently.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SpotifyAudioOutput SpotifyAudioOutput;

typedef enum {
  AUDIO_BACKEND_PIPEWIRE,
  AUDIO_BACKEND_PULSE,
  AUDIO_BACKEND_ALSA,
  AUDIO_BACKEND_WASAPI,
  AUDIO_BACKEND_NONE,
} AudioBackendKind;

typedef struct {
  gboolean (*open)    (SpotifyAudioOutput *self, gint sample_rate, gint channels);
  gsize    (*write)   (SpotifyAudioOutput *self, const gint16 *samples, gsize n_frames);
  void     (*set_volume) (SpotifyAudioOutput *self, gdouble volume_0_to_1);
  void     (*drain)   (SpotifyAudioOutput *self);
  void     (*close)   (SpotifyAudioOutput *self);
} AudioBackendVtable;

struct _SpotifyAudioOutput {
  AudioBackendKind          kind;
  const AudioBackendVtable *vtable;
  gpointer                  backend_data;   /* backend-private state */
};

/* Probes available backends in priority order and opens the first
 * one that actually works at runtime:
 *   nightly: PipeWire -> PulseAudio -> ALSA
 *   stable:  PulseAudio -> ALSA
 * (Windows build substitutes WASAPI as the sole candidate.)
 */
SpotifyAudioOutput *spotifygtk_output_open (gint sample_rate, gint channels);

gsize spotifygtk_output_write      (SpotifyAudioOutput *self, const gint16 *samples, gsize n_frames);
void  spotifygtk_output_set_volume (SpotifyAudioOutput *self, gdouble volume_0_to_1);
void  spotifygtk_output_drain      (SpotifyAudioOutput *self);
void  spotifygtk_output_close      (SpotifyAudioOutput *self);

const gchar *spotifygtk_output_backend_name (AudioBackendKind kind);

/* Backend constructors — only called internally by output.c's probe
 * logic, declared here so each backend file stays self-contained. */
gboolean output_pipewire_try_open (SpotifyAudioOutput *self, gint rate, gint channels);
gboolean output_pulse_try_open    (SpotifyAudioOutput *self, gint rate, gint channels);
gboolean output_alsa_try_open     (SpotifyAudioOutput *self, gint rate, gint channels);
gboolean output_wasapi_try_open   (SpotifyAudioOutput *self, gint rate, gint channels);

G_END_DECLS

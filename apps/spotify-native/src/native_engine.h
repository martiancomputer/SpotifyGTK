#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  SPOTIFYGTK_ENGINE_IDLE,
  SPOTIFYGTK_ENGINE_CONNECTING,
  SPOTIFYGTK_ENGINE_BUFFERING,
  SPOTIFYGTK_ENGINE_PLAYING,
} SpotifyNativeEngineStage;

typedef void (*SpotifyNativeEngineProgressFunc) (SpotifyNativeEngineStage stage,
                                                 const gchar *message,
                                                 gpointer user_data);

typedef struct _SpotifyNativeEngineControl SpotifyNativeEngineControl;

SpotifyNativeEngineControl *spotifygtk_native_engine_control_new (void);
void spotifygtk_native_engine_control_free (SpotifyNativeEngineControl *control);
void spotifygtk_native_engine_control_pause (SpotifyNativeEngineControl *control);
void spotifygtk_native_engine_control_resume (SpotifyNativeEngineControl *control);
gboolean spotifygtk_native_engine_control_is_paused (SpotifyNativeEngineControl *control);

/* Output gain, 0.0 to 1.0. Safe to call from any thread and before the
 * output device exists: the value is stored and applied by the audio worker
 * once it opens, and again whenever it changes. */
void    spotifygtk_native_engine_control_set_volume (SpotifyNativeEngineControl *control,
                                                     gdouble volume_0_to_1);
gdouble spotifygtk_native_engine_control_get_volume (SpotifyNativeEngineControl *control);

/* Runs one complete native playback attempt. Must not be called on the GTK
 * main thread; the player service owns the worker task that invokes it. The
 * progress callback is optional and is invoked on the worker thread, so UI
 * callers must marshal it back to their main context. */
gboolean spotifygtk_native_engine_run (GCancellable *cancellable,
                                       SpotifyNativeEngineProgressFunc progress,
                                       gpointer progress_data,
                                       SpotifyNativeEngineControl *control,
                                       const gchar *track_uri);

G_END_DECLS

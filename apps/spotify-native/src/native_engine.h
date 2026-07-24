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

/* Equaliser gains, dB per band (SPOTIFYGTK_EQ_BANDS). Pushed like volume;
 * the filter state lives in the control so it persists across buffers. */
void    spotifygtk_native_engine_control_set_eq (SpotifyNativeEngineControl *control,
                                                 const gdouble *gains_db, gboolean enabled);

/* Playback position, reported by the audio worker and read by the UI.
 *
 * The worker calls report_position() as it writes PCM to the device, passing
 * the cumulative frames written and the stream's sample rate; get_position_ms
 * turns that into milliseconds. Both are lock-guarded, so the UI can poll from
 * its own thread. Position resets naturally because each playback attempt gets
 * a fresh control. */
void   spotifygtk_native_engine_control_report_position (SpotifyNativeEngineControl *control,
                                                         guint64 played_frames,
                                                         gint    sample_rate);
gint64 spotifygtk_native_engine_control_get_position_ms (SpotifyNativeEngineControl *control);

/*
 * Seek. The UI calls request_seek() with a target in milliseconds; the engine
 * checks seek_pending() at its wait points and consumes the request with
 * take_seek() when it is ready to act. Consuming clears the flag, so a request
 * is honoured once; a newer request made before the engine acts replaces the
 * target. Safe from any thread.
 */
void     spotifygtk_native_engine_control_request_seek (SpotifyNativeEngineControl *control,
                                                        gint64 target_ms);
gboolean spotifygtk_native_engine_control_seek_pending (SpotifyNativeEngineControl *control);
gboolean spotifygtk_native_engine_control_take_seek (SpotifyNativeEngineControl *control,
                                                     gint64 *out_target_ms);

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

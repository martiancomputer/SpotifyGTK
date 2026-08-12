#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  SPOTIFYGTK_ENGINE_IDLE,
  SPOTIFYGTK_ENGINE_CONNECTING,
  SPOTIFYGTK_ENGINE_BUFFERING,
  SPOTIFYGTK_ENGINE_PLAYING,
  /* The server has no playable file for this track. Distinct from a failure:
   * nothing is wrong and retrying cannot help, so the caller should move on
   * rather than stop. */
  SPOTIFYGTK_ENGINE_UNAVAILABLE,
} SpotifyNativeEngineStage;

typedef void (*SpotifyNativeEngineProgressFunc) (SpotifyNativeEngineStage stage,
                                                 const gchar *message,
                                                 gpointer user_data);

typedef struct _SpotifyNativeEngineControl SpotifyNativeEngineControl;

SpotifyNativeEngineControl *spotifygtk_native_engine_control_new (void);
/* The sink keeps a reference of its own; see the struct comment in main.c. */
SpotifyNativeEngineControl *spotifygtk_native_engine_control_ref (SpotifyNativeEngineControl *control);
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
/* Device rate to open the output at; 0 (default) follows the stream and does
 * no conversion. Any other rate engages the resampler. */
void    spotifygtk_native_engine_control_set_output_rate (SpotifyNativeEngineControl *control,
                                                          gint rate_hz);

void    spotifygtk_native_engine_control_set_eq (SpotifyNativeEngineControl *control,
                                                 const gdouble *gains_db, gboolean enabled);

/*
 * Shared with the audio sink, which applies these as it writes each frame.
 *
 * They were private to the engine while the engine also owned the device. The
 * device now outlives any one track (see audio/sink.h), so whatever feeds it
 * has to be able to reach the control of whichever track it is playing.
 *
 * wait() is the pause gate: it blocks while paused and returns FALSE if the
 * track was cancelled.
 */
gboolean spotifygtk_native_engine_control_wait (SpotifyNativeEngineControl *control,
                                                GCancellable *cancellable);
void     spotifygtk_native_engine_control_apply_volume (SpotifyNativeEngineControl *control,
                                                        gint16 *samples, gsize n_frames,
                                                        gint channels);
void     spotifygtk_native_engine_control_apply_eq (SpotifyNativeEngineControl *control,
                                                    gint16 *samples, gsize n_frames,
                                                    gint channels, gint rate);
gint     spotifygtk_native_engine_control_get_output_rate (SpotifyNativeEngineControl *control);

/* The sink slot this track occupies. Set by the engine, read by the player
 * service to work out which of its controls is the audible one. */
void    spotifygtk_native_engine_control_set_sink_seq (SpotifyNativeEngineControl *control,
                                                       guint64 seq);
guint64 spotifygtk_native_engine_control_get_sink_seq (SpotifyNativeEngineControl *control);

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

/*
 * Add or remove a track from Liked Songs.
 *
 * Goes over the collection v2 service on the live AP connection, which is
 * additive -- it names only what changes. Returns FALSE without doing anything
 * if there is no session yet: Mercury rides the AP connection, so nothing can
 * be written before something has played.
 *
 * `callback` runs on the engine thread, not the main loop.
 */
typedef void (*SpotifyNativeLikeCallback) (gboolean ok, guint16 status,
                                           gpointer user_data);

gboolean spotifygtk_native_engine_set_track_liked (const gchar *track_uri,
                                                   gboolean liked,
                                                   SpotifyNativeLikeCallback callback,
                                                   gpointer user_data);

G_END_DECLS

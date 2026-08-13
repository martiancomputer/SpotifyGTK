#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_PLAYER_SERVICE (spotifygtk_player_service_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyNativePlayerService, spotifygtk_player_service,
                      SPOTIFYGTK, PLAYER_SERVICE, GObject)

typedef enum {
  SPOTIFYGTK_PLAYER_IDLE,
  SPOTIFYGTK_PLAYER_CONNECTING,
  SPOTIFYGTK_PLAYER_BUFFERING,
  SPOTIFYGTK_PLAYER_PLAYING,
  SPOTIFYGTK_PLAYER_PAUSED,
  SPOTIFYGTK_PLAYER_STOPPING,
  SPOTIFYGTK_PLAYER_ERROR,
  /* The server has no file for this track; the UI moves to the next one. */
  SPOTIFYGTK_PLAYER_UNAVAILABLE,
} SpotifyNativePlayerState;

SpotifyNativePlayerService *spotifygtk_player_service_new (void);
gboolean spotifygtk_player_service_start (SpotifyNativePlayerService *self, GError **error);
gboolean spotifygtk_player_service_start_uri (SpotifyNativePlayerService *self,
                                              const gchar *track_uri,
                                              GError **error);
void     spotifygtk_player_service_stop  (SpotifyNativePlayerService *self);

/*
 * Throw away audio already decoded for tracks that are on their way out.
 *
 * Call before starting a track the listener picked directly. A gapless
 * handover must not: the queued tail of the outgoing track is the thing that
 * makes it gapless.
 */
void     spotifygtk_player_service_drop_queued_audio (SpotifyNativePlayerService *self);
void     spotifygtk_player_service_pause (SpotifyNativePlayerService *self);
void     spotifygtk_player_service_resume (SpotifyNativePlayerService *self);
gboolean spotifygtk_player_service_is_paused (SpotifyNativePlayerService *self);

/* Reposition the current track to `position_ms`. No-op if nothing is playing.
 * The engine seeks asynchronously; position-changed will report the new spot
 * once it lands. */
void     spotifygtk_player_service_seek (SpotifyNativePlayerService *self, gint64 position_ms);

/* Output gain, 0-100. Remembered across track changes: each playback
 * creates a fresh engine control, so the value has to be re-applied rather
 * than living only in the engine. */
void spotifygtk_player_service_set_volume (SpotifyNativePlayerService *self, gint percent);
/* Device rate for playback; 0 follows the stream. Applies from the next
 * track, since the output device is opened once per track. */
void spotifygtk_player_service_set_output_rate (SpotifyNativePlayerService *self, gint hz);

void spotifygtk_player_service_set_eq (SpotifyNativePlayerService *self,
                                       const gdouble *gains_db, gboolean enabled);
gint spotifygtk_player_service_get_volume (SpotifyNativePlayerService *self);
gboolean spotifygtk_player_service_is_active (SpotifyNativePlayerService *self);
SpotifyNativePlayerState spotifygtk_player_service_get_state (SpotifyNativePlayerService *self);

/* Signals:
 * - state-changed (SpotifyNativePlayerState state, const gchar *message)
 * - position-changed (gint64 position_ms)
 *
 *   position-changed fires ~4x/second while a track is active, off the engine
 *   control the audio worker updates. The service does not know the track's
 *   duration — the UI pairs this with the duration it already has.
 */

G_END_DECLS

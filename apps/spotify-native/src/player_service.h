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
  SPOTIFYGTK_PLAYER_STOPPING,
  SPOTIFYGTK_PLAYER_ERROR,
} SpotifyNativePlayerState;

SpotifyNativePlayerService *spotifygtk_player_service_new (void);
gboolean spotifygtk_player_service_start (SpotifyNativePlayerService *self, GError **error);
gboolean spotifygtk_player_service_start_uri (SpotifyNativePlayerService *self,
                                              const gchar *track_uri,
                                              GError **error);
void     spotifygtk_player_service_stop  (SpotifyNativePlayerService *self);
void     spotifygtk_player_service_pause (SpotifyNativePlayerService *self);
void     spotifygtk_player_service_resume (SpotifyNativePlayerService *self);
gboolean spotifygtk_player_service_is_paused (SpotifyNativePlayerService *self);
gboolean spotifygtk_player_service_is_active (SpotifyNativePlayerService *self);
SpotifyNativePlayerState spotifygtk_player_service_get_state (SpotifyNativePlayerService *self);

/* Signal: state-changed (SpotifyNativePlayerState state, const gchar *message) */

G_END_DECLS

#pragma once

#include <glib-object.h>
#include "api.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_PLAYER (spotifygtk_player_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyPlayer, spotifygtk_player, SPOTIFYGTK, PLAYER, GObject)

SpotifyPlayer *spotifygtk_player_new           (SpotifyApi *api);
void           spotifygtk_player_start_polling (SpotifyPlayer *self, guint interval_ms);
void           spotifygtk_player_stop_polling  (SpotifyPlayer *self);

void spotifygtk_player_play_pause (SpotifyPlayer *self);
void spotifygtk_player_next       (SpotifyPlayer *self);
void spotifygtk_player_previous   (SpotifyPlayer *self);
void spotifygtk_player_set_volume (SpotifyPlayer *self, gint percent);
void spotifygtk_player_seek       (SpotifyPlayer *self, gint64 position_ms);

gboolean     spotifygtk_player_is_playing       (SpotifyPlayer *self);
gint64       spotifygtk_player_get_position     (SpotifyPlayer *self);
gint64       spotifygtk_player_get_duration     (SpotifyPlayer *self);
gint         spotifygtk_player_get_volume       (SpotifyPlayer *self);
const gchar *spotifygtk_player_get_track_name   (SpotifyPlayer *self);
const gchar *spotifygtk_player_get_artist_name  (SpotifyPlayer *self);
const gchar *spotifygtk_player_get_album_art_url (SpotifyPlayer *self);

/* Signal: "state-changed" */

G_END_DECLS

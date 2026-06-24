#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include "auth.h"

G_BEGIN_DECLS

#define SPOTIFY_API_BASE "https://api.spotify.com/v1"

#define SPOTIFYGTK_TYPE_API (spotifygtk_api_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyApi, spotifygtk_api, SPOTIFYGTK, API, GObject)

typedef void (*SpotifyApiCallback) (SpotifyApi *api, JsonObject *result,
                                    GError *error, gpointer user_data);

SpotifyApi *spotifygtk_api_new (SpotifyAuth *auth);

void spotifygtk_api_get_playback_state  (SpotifyApi *self, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_play                (SpotifyApi *self, const gchar *context_uri, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_play_track          (SpotifyApi *self, const gchar *track_uri, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_pause               (SpotifyApi *self, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_next                (SpotifyApi *self, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_previous            (SpotifyApi *self, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_set_volume          (SpotifyApi *self, gint percent, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_seek                (SpotifyApi *self, gint64 position_ms, SpotifyApiCallback cb, gpointer data);

void spotifygtk_api_get_user_playlists  (SpotifyApi *self, gint limit, gint offset, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_get_playlist_tracks (SpotifyApi *self, const gchar *playlist_id, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_get_saved_tracks    (SpotifyApi *self, gint limit, gint offset, SpotifyApiCallback cb, gpointer data);

void spotifygtk_api_search              (SpotifyApi *self, const gchar *query, const gchar *types, SpotifyApiCallback cb, gpointer data);
void spotifygtk_api_get_current_user    (SpotifyApi *self, SpotifyApiCallback cb, gpointer data);

G_END_DECLS

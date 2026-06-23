#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SPOTIFY_SCOPES \
  "user-read-playback-state " \
  "user-modify-playback-state " \
  "user-read-currently-playing " \
  "user-read-private " \
  "user-read-email " \
  "playlist-read-private " \
  "playlist-read-collaborative " \
  "user-library-read " \
  "user-top-read " \
  "user-follow-read " \
  "streaming"

#define SPOTIFYGTK_TYPE_AUTH (spotifygtk_auth_get_type ())

G_DECLARE_FINAL_TYPE (SpotifyAuth, spotifygtk_auth, SPOTIFYGTK, AUTH, GObject)

SpotifyAuth *spotifygtk_auth_new            (void);
void         spotifygtk_auth_begin          (SpotifyAuth *self);
gboolean     spotifygtk_auth_has_valid_token (SpotifyAuth *self);
const gchar *spotifygtk_auth_get_token      (SpotifyAuth *self);
void         spotifygtk_auth_refresh        (SpotifyAuth *self);
void         spotifygtk_auth_revoke         (SpotifyAuth *self);

/* Signal: "completed" (gboolean success) */

G_END_DECLS

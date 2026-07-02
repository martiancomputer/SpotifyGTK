/*
 * native_auth.h — OAuth 2.0 PKCE for AP-level login, using Spotify's
 * internal "keymaster" client_id.
 *
 * This is a SEPARATE credential from anything apps/spotify-connect
 * produces. A token from spotify-connect's OAuth flow (dashboard-
 * registered client_id, Web API scopes like user-read-playback-state)
 * was tested against the AP's AUTHENTICATION_SPOTIFY_TOKEN login and
 * rejected within ~100ms, no structured error, just a closed
 * connection. That's consistent with a client_id/scope mismatch at
 * the credential-validation layer, not a bug in the login message
 * itself (which was already confirmed structurally correct via
 * tests/test_login_encoding.c).
 *
 * The fix, verified against librespot's own source rather than
 * guessed: librespot's real client (src/main.rs, --enable-oauth path)
 * doesn't use a dashboard-registered client_id at all for this -- it
 * uses KEYMASTER_CLIENT_ID = "65b708073fc0480ea92a077233ca87bd"
 * (core/src/config.rs), Spotify's own internal client_id that their
 * official apps use, requesting a specific scope list (OAUTH_SCOPES
 * in src/main.rs -- app-remote-control, streaming, playlist-*,
 * user-follow-*, user-library-*, user-modify*, etc., which is NOT the
 * same list a dashboard app would typically request).
 *
 * Redirect URI: confirmed against librespot-oauth's own worked
 * example (oauth/examples/oauth_sync.rs), not assumed --
 * http://127.0.0.1:8898/login, port 8898, path /login specifically.
 * This must be pre-registered against the keymaster client_id on
 * Spotify's side (we can't see or change that registration -- if
 * Spotify ever changes it, this would need updating to match).
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* Verified against librespot's core/src/config.rs -- this is
 * Spotify's own internal client_id, not something registered via
 * developer.spotify.com. */
#define NATIVE_AUTH_CLIENT_ID   "65b708073fc0480ea92a077233ca87bd"

/* Verified against librespot-oauth's own example
 * (oauth/examples/oauth_sync.rs) as a concrete working redirect. */
#define NATIVE_AUTH_REDIRECT_PORT 8898
#define NATIVE_AUTH_REDIRECT_URI  "http://127.0.0.1:8898/login"

/* Verified against librespot's src/main.rs OAUTH_SCOPES constant,
 * transcribed in full rather than trimmed to what "seemed relevant". */
#define NATIVE_AUTH_SCOPES \
  "app-remote-control " \
  "playlist-modify " \
  "playlist-modify-private " \
  "playlist-modify-public " \
  "playlist-read " \
  "playlist-read-collaborative " \
  "playlist-read-private " \
  "streaming " \
  "ugc-image-upload " \
  "user-follow-modify " \
  "user-follow-read " \
  "user-library-modify " \
  "user-library-read " \
  "user-modify " \
  "user-modify-playback-state " \
  "user-modify-private " \
  "user-personalized " \
  "user-read-birthdate " \
  "user-read-currently-playing " \
  "user-read-email " \
  "user-read-play-history " \
  "user-read-playback-position " \
  "user-read-playback-state " \
  "user-read-private " \
  "user-read-recently-played " \
  "user-top-read"

#define NATIVEAUTH_TYPE_AUTH (native_auth_get_type ())
G_DECLARE_FINAL_TYPE (NativeAuth, native_auth, NATIVEAUTH, AUTH, GObject)

NativeAuth *native_auth_new (void);

/* Opens the browser to Spotify's consent screen; result arrives via
 * the "completed" signal, same shape as spotify-connect's auth.c. */
void     native_auth_begin (NativeAuth *self);

gboolean     native_auth_has_valid_token (NativeAuth *self);
const gchar *native_auth_get_token       (NativeAuth *self);
void         native_auth_refresh         (NativeAuth *self);

/* Signal: "completed" (gboolean success) */

G_END_DECLS

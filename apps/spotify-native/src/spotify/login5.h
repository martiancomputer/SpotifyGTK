/*
 * login5.h — exchanges the AP login's reusable credential for a
 * short-lived bearer token usable against spclient's HTTPS API.
 *
 * This is the piece that closes the gap discovered while researching
 * streaming: the OAuth token native_auth.c obtained (and that got us
 * through AP login) is NOT what spclient's Authorization header
 * wants. What it wants comes from here, built from the
 * reusable_auth_credentials APWelcome handed back after a successful
 * login (spotifygtk_ap_session_get_reusable_creds()) -- NOT the
 * original OAuth token. See research/playback/ for the full finding.
 *
 * Confirmed against librespot's core/src/login5.rs: `auth_token()`
 * builds a LoginRequest with a StoredCredential
 * (username + the reusable creds blob) and POSTs it to
 * login5.spotify.com; a successful LoginOk response's access_token
 * (plus a hardcoded "Bearer" token_type -- not part of the wire
 * response) is what gets used downstream.
 */

#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define LOGIN5_URL "https://login5.spotify.com/v3/login"

typedef void (*Login5Callback) (const gchar *access_token /* nullable on failure */,
                                gint32 expires_in_seconds,
                                GError *error,
                                gpointer user_data);

#define SPOTIFYGTK_TYPE_LOGIN5 (spotifygtk_login5_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyLogin5, spotifygtk_login5, SPOTIFYGTK, LOGIN5, GObject)

SpotifyLogin5 *spotifygtk_login5_new (void);
void spotifygtk_login5_set_cancellable (SpotifyLogin5 *, GCancellable *);

/* client_id: NATIVE_AUTH_CLIENT_ID (keymaster). device_id: any stable
 * per-install identifier is fine -- librespot uses a random UUID
 * generated once and cached; a simpler fixed string works for now
 * (real device-identity handling is a separate, later concern). */
void spotifygtk_login5_auth_token (SpotifyLogin5 *self,
                                   const gchar   *client_id,
                                   const gchar   *device_id,
                                   const gchar   *username,       /* nullable */
                                   const guint8  *reusable_creds,
                                   gsize          reusable_creds_len,
                                   guint64        reusable_creds_type,
                                   const gchar   *client_token,   /* nullable */
                                   Login5Callback callback,
                                   gpointer       user_data);

G_END_DECLS

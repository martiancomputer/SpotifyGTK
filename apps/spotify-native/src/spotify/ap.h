/*
 * ap.h — Spotify Access Point connection.
 *
 * The AP connection is a raw TCP socket (resolved via DNS SRV lookup
 * on _spotify-client._tcp) that carries a Diffie-Hellman handshake
 * followed by a Shannon-encrypted binary protocol. Mercury (pub/sub
 * metadata), audio key exchange, and Spotify Connect control all run
 * over this one connection, multiplexed by a 1-byte command ID + 2-byte
 * length header per packet.
 *
 * STATUS: connection + framing scaffolding. The DH handshake here
 * depends on shannon.c being complete (see shannon.h) before it can
 * actually authenticate against Spotify's servers.
 */

#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include "shannon.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_AP_SESSION (spotifygtk_ap_session_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyApSession, spotifygtk_ap_session, SPOTIFYGTK, AP_SESSION, GObject)

typedef enum {
  AP_CMD_SECRET_BLOCK    = 0x02,
  AP_CMD_PING            = 0x04,
  AP_CMD_STREAM_CHUNK    = 0x08,
  AP_CMD_STREAM_CHUNK_RES= 0x09,
  AP_CMD_CHANNEL_ERROR   = 0x0a,
  AP_CMD_CHANNEL_ABORT   = 0x0b,
  AP_CMD_REQUEST_KEY     = 0x0c,
  AP_CMD_AES_KEY         = 0x0d,
  AP_CMD_AES_KEY_ERROR   = 0x0e,
  AP_CMD_LOGIN           = 0xab,  /* ClientResponseEncrypted */
  AP_CMD_APWELCOME       = 0xac,  /* login success */
  AP_CMD_AUTH_FAILURE    = 0xad,  /* login failure, APLoginFailed */
  AP_CMD_MERCURY_REQ     = 0xb2,
  AP_CMD_MERCURY_SUB     = 0xb3,
  AP_CMD_MERCURY_UNSUB   = 0xb4,
  AP_CMD_MERCURY_EVENT   = 0xb5,
} ApCommandId;

typedef void (*ApPacketHandler) (SpotifyApSession *session,
                                 ApCommandId       cmd,
                                 const guint8     *payload,
                                 gsize             len,
                                 gpointer          user_data);

SpotifyApSession *spotifygtk_ap_session_new (void);

/* Resolves an AP host via SRV lookup, connects, and performs the
 * Diffie-Hellman + Shannon handshake. Calls back on completion.
 *
 * NOTE: access_token is currently stored but unused -- login became
 * its own separate explicit step (spotifygtk_ap_session_login(),
 * called after both this succeeds and start_receiving() is running),
 * rather than something connect() does automatically. Kept as a
 * parameter for now since removing it is an API change with no
 * functional benefit at this exact point; may be dropped later if it
 * stays unused. */
void spotifygtk_ap_session_connect (SpotifyApSession *self,
                                    const gchar      *access_token,
                                    GAsyncReadyCallback callback,
                                    gpointer            user_data);

gboolean spotifygtk_ap_session_connect_finish (SpotifyApSession *self,
                                               GAsyncResult     *result,
                                               GError          **error);

/* Send a framed, Shannon-encrypted packet. */
void spotifygtk_ap_session_send (SpotifyApSession *self,
                                 ApCommandId       cmd,
                                 const guint8     *payload,
                                 gsize             len);

/* Starts the async post-handshake receive loop -- reads one framed,
 * Shannon-decrypted packet at a time, MAC-verifies it, and dispatches
 * to whatever handler is registered for its command ID via
 * spotifygtk_ap_session_set_handler(), then immediately schedules the
 * next read. Call once, right after spotifygtk_ap_session_connect()
 * succeeds. Without this running, nothing sent via _send() will ever
 * see a response delivered back to a handler -- including login. */
void spotifygtk_ap_session_start_receiving (SpotifyApSession *self);

/* Register a handler for incoming packets of a given command ID. */
void spotifygtk_ap_session_set_handler (SpotifyApSession *self,
                                        ApCommandId       cmd,
                                        ApPacketHandler   handler,
                                        gpointer          user_data);

/*
 * Sends the post-handshake login (ClientResponseEncrypted, using
 * AUTHENTICATION_SPOTIFY_TOKEN per librespot's authentication.rs --
 * reuses the OAuth access token from spotify-connect's auth.c rather
 * than needing a raw username/password). Requires
 * spotifygtk_ap_session_start_receiving() to already be running, since
 * the result arrives via the receive loop, not synchronously.
 *
 * callback fires exactly once: with success=TRUE and a non-NULL
 * username on APWelcome, or success=FALSE (username NULL) on
 * AuthFailure or any transport error.
 */
typedef void (*ApLoginCallback) (gboolean success, const gchar *username,
                                 GError *error, gpointer user_data);

void spotifygtk_ap_session_login (SpotifyApSession *self,
                                  const gchar      *spotify_username,
                                  const gchar      *oauth_access_token,
                                  ApLoginCallback   callback,
                                  gpointer          user_data);

void spotifygtk_ap_session_disconnect (SpotifyApSession *self);

G_END_DECLS

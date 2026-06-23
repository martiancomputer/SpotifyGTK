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
 * Diffie-Hellman + Shannon handshake. Calls back on completion. */
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

/* Register a handler for incoming packets of a given command ID. */
void spotifygtk_ap_session_set_handler (SpotifyApSession *self,
                                        ApCommandId       cmd,
                                        ApPacketHandler   handler,
                                        gpointer          user_data);

void spotifygtk_ap_session_disconnect (SpotifyApSession *self);

G_END_DECLS

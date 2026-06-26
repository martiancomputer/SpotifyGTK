/*
 * audio_key.h — Per-track audio decryption key exchange.
 *
 * Each track's audio file on the CDN is AES-128-CTR encrypted with a
 * key that must be requested per-session over the AP connection
 * (AP_CMD_REQUEST_KEY / AP_CMD_AES_KEY). The request takes a track
 * GID + file ID; the response is a 16-byte AES key tied to that
 * session's auth.
 */

#pragma once

#include <glib-object.h>
#include "ap.h"

G_BEGIN_DECLS

#define AUDIO_KEY_LEN 16

typedef void (*AudioKeyCallback) (const guint8 key[AUDIO_KEY_LEN], GError *error, gpointer user_data);

#define SPOTIFYGTK_TYPE_AUDIO_KEY_CLIENT (spotifygtk_audio_key_client_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyAudioKeyClient, spotifygtk_audio_key_client,
                      SPOTIFYGTK, AUDIO_KEY_CLIENT, GObject)

SpotifyAudioKeyClient *spotifygtk_audio_key_client_new (SpotifyApSession *ap_session);

/* track_gid / file_id are raw 16/20-byte identifiers as found in track
 * metadata (Mercury responses), not base62 Spotify URIs. */
void spotifygtk_audio_key_request (SpotifyAudioKeyClient *self,
                                   const guint8 *track_gid, gsize track_gid_len,
                                   const guint8 *file_id,   gsize file_id_len,
                                   AudioKeyCallback callback, gpointer user_data);

G_END_DECLS

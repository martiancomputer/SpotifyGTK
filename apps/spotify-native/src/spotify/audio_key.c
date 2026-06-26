/*
 * audio_key.c — Per-track audio decryption key exchange.
 *
 * Request packet layout (AP_CMD_REQUEST_KEY):
 *   [file_id (20 bytes)][track_gid (16 bytes)][4-byte sequence, BE]
 * Response (AP_CMD_AES_KEY): [4-byte sequence, BE][16-byte AES key]
 * Error (AP_CMD_AES_KEY_ERROR): [4-byte sequence, BE][2-byte error code]
 *
 * This is a request/response pair over the same framed AP connection
 * as Mercury — no cryptographic work happens in this file itself
 * (the AES key it retrieves is *used* by cdn.c, not generated here),
 * so unlike shannon.c this is safe to implement in full.
 */

#include "config.h"
#include "audio_key.h"
#include <string.h>

struct _SpotifyAudioKeyClient {
  GObject           parent_instance;
  SpotifyApSession *ap_session;
  guint32           next_seq;
  GHashTable       *pending;       /* seq -> AudioKeyCallback */
  GHashTable       *pending_data;
};

G_DEFINE_FINAL_TYPE (SpotifyAudioKeyClient, spotifygtk_audio_key_client, G_TYPE_OBJECT)

void
spotifygtk_audio_key_request (SpotifyAudioKeyClient *self,
                              const guint8 *track_gid, gsize track_gid_len,
                              const guint8 *file_id,   gsize file_id_len,
                              AudioKeyCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_AUDIO_KEY_CLIENT (self));

  guint32 seq = self->next_seq++;

  GByteArray *buf = g_byte_array_new ();
  g_byte_array_append (buf, file_id, file_id_len);
  g_byte_array_append (buf, track_gid, track_gid_len);

  guint8 seq_be[4] = {
    (guint8) (seq >> 24), (guint8) (seq >> 16), (guint8) (seq >> 8), (guint8) seq
  };
  g_byte_array_append (buf, seq_be, sizeof (seq_be));

  if (callback) {
    g_hash_table_insert (self->pending,      g_memdup2 (&seq, sizeof (seq)), callback);
    g_hash_table_insert (self->pending_data, g_memdup2 (&seq, sizeof (seq)), user_data);
  }

  spotifygtk_ap_session_send (self->ap_session, AP_CMD_REQUEST_KEY, buf->data, buf->len);
  g_byte_array_free (buf, TRUE);
}

/* Dispatch — wired by the owner via
 * ap_session_set_handler(AP_CMD_AES_KEY, ...) and
 * ap_session_set_handler(AP_CMD_AES_KEY_ERROR, ...) once a live AP
 * session exists. Kept as free functions here so player.c (or
 * whatever owns the session) can route both command IDs to us. */

void
spotifygtk_audio_key_handle_response (SpotifyAudioKeyClient *self,
                                      const guint8 *payload, gsize len)
{
  if (len < 4 + AUDIO_KEY_LEN) return;

  guint32 seq = ((guint32) payload[0] << 24) | ((guint32) payload[1] << 16) |
                ((guint32) payload[2] << 8)  |  (guint32) payload[3];

  AudioKeyCallback cb   = g_hash_table_lookup (self->pending, &seq);
  gpointer         data = g_hash_table_lookup (self->pending_data, &seq);
  if (cb) cb (payload + 4, NULL, data);

  g_hash_table_remove (self->pending, &seq);
  g_hash_table_remove (self->pending_data, &seq);
}

void
spotifygtk_audio_key_handle_error (SpotifyAudioKeyClient *self,
                                   const guint8 *payload, gsize len)
{
  if (len < 4) return;
  guint32 seq = ((guint32) payload[0] << 24) | ((guint32) payload[1] << 16) |
                ((guint32) payload[2] << 8)  |  (guint32) payload[3];

  AudioKeyCallback cb   = g_hash_table_lookup (self->pending, &seq);
  gpointer         data = g_hash_table_lookup (self->pending_data, &seq);
  if (cb) {
    GError *err = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Audio key request denied");
    cb (NULL, err, data);
    g_error_free (err);
  }
  g_hash_table_remove (self->pending, &seq);
  g_hash_table_remove (self->pending_data, &seq);
}

static void
spotifygtk_audio_key_client_dispose (GObject *object)
{
  SpotifyAudioKeyClient *self = SPOTIFYGTK_AUDIO_KEY_CLIENT (object);
  g_clear_object (&self->ap_session);
  g_clear_pointer (&self->pending,      g_hash_table_unref);
  g_clear_pointer (&self->pending_data, g_hash_table_unref);
  G_OBJECT_CLASS (spotifygtk_audio_key_client_parent_class)->dispose (object);
}

static void
spotifygtk_audio_key_client_class_init (SpotifyAudioKeyClientClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_audio_key_client_dispose;
}

static void
spotifygtk_audio_key_client_init (SpotifyAudioKeyClient *self)
{
  self->pending      = g_hash_table_new_full (g_int_hash, g_int_equal, g_free, NULL);
  self->pending_data = g_hash_table_new_full (g_int_hash, g_int_equal, g_free, NULL);
}

SpotifyAudioKeyClient *
spotifygtk_audio_key_client_new (SpotifyApSession *ap_session)
{
  SpotifyAudioKeyClient *self = g_object_new (SPOTIFYGTK_TYPE_AUDIO_KEY_CLIENT, NULL);
  self->ap_session = g_object_ref (ap_session);
  return self;
}

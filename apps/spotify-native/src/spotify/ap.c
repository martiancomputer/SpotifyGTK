/*
 * ap.c — Access Point connection implementation.
 *
 * STATUS: SRV resolution, TCP connect, packet framing, AND the
 * Diffie-Hellman handshake are all real now. The handshake (see
 * perform_handshake() below) was ported from librespot's
 * core/src/connection/handshake.rs against the verified constants in
 * handshake_constants.h -- not yet tested against a live Spotify
 * server (this sandbox can't reach ap.spotify.com), so treat it as
 * "should be correct per the reference" rather than "confirmed
 * working" until run for real.
 */

#include "config.h"
#include "ap.h"
#include "dh.h"
#include "handshake_crypto.h"
#include "protobuf_min.h"

#include <gio/gio.h>
#include <string.h>
#if HAVE_OPENSSL
#include <openssl/rand.h>
#endif

#define AP_FALLBACK_HOST "ap.spotify.com"
#define AP_FALLBACK_PORT 4070

struct _SpotifyApSession {
  GObject          parent_instance;

  GSocketClient   *client;
  GSocketConnection *connection;
  ShannonCipher    send_cipher;
  ShannonCipher    recv_cipher;
  guint32          send_nonce;
  guint32          recv_nonce;

  GHashTable      *handlers;   /* ApCommandId -> ApPacketHandler */
  GHashTable      *handler_data;

  gboolean         connected;
};

G_DEFINE_FINAL_TYPE (SpotifyApSession, spotifygtk_ap_session, G_TYPE_OBJECT)

/* ── DNS SRV resolution ──────────────────────────────────────────────────
 * Spotify publishes AP hosts via _spotify-client._tcp SRV records.
 * GLib's GResolver exposes this directly — no custom DNS code needed. */

static void
on_srv_resolved (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data;
  g_autoptr(GError) err = NULL;

  GList *targets = g_resolver_lookup_service_finish (G_RESOLVER (source), result, &err);
  if (!targets || err) {
    g_message ("SRV lookup failed (%s), falling back to %s:%d",
              err ? err->message : "no records", AP_FALLBACK_HOST, AP_FALLBACK_PORT);
    g_task_return_pointer (task,
      g_strdup_printf ("%s:%d", AP_FALLBACK_HOST, AP_FALLBACK_PORT), g_free);
    return;
  }

  GSrvTarget *best = targets->data;  /* GResolver already sorts by priority/weight */
  g_autofree gchar *result_str =
    g_strdup_printf ("%s:%d", g_srv_target_get_hostname (best), g_srv_target_get_port (best));
  g_task_return_pointer (task, g_strdup (result_str), g_free);

  g_resolver_free_targets (targets);
}

static void
resolve_ap_host (GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new (NULL, NULL, callback, user_data);
  GResolver *resolver = g_resolver_get_default ();
  g_resolver_lookup_service_async (resolver, "spotify-client", "tcp", "spotify.com",
                                   NULL, on_srv_resolved, task);
}

/* ── Packet framing ──────────────────────────────────────────────────────
 * Wire format per packet (post-handshake): [1 byte cmd][2 bytes BE len][payload]
 * the whole thing Shannon-encrypted, with a 4-byte MAC appended per the
 * cipher's internal counter. This framing is independent of whether the
 * cipher's round function is complete. */

void
spotifygtk_ap_session_send (SpotifyApSession *self, ApCommandId cmd,
                            const guint8 *payload, gsize len)
{
  g_return_if_fail (SPOTIFYGTK_IS_AP_SESSION (self));
  if (!self->connected) {
    g_warning ("ap_session_send called before handshake completed");
    return;
  }

  guint8 header[3];
  header[0] = (guint8) cmd;
  header[1] = (guint8) ((len >> 8) & 0xff);
  header[2] = (guint8) (len & 0xff);

  g_autofree guint8 *frame = g_malloc (3 + len);
  memcpy (frame, header, 3);
  if (len > 0) memcpy (frame + 3, payload, len);

  shannon_encrypt (&self->send_cipher, frame, 3 + len);

  GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (self->connection));
  g_output_stream_write_all (out, frame, 3 + len, NULL, NULL, NULL);

  guint8 mac[4];
  shannon_finish (&self->send_cipher, mac, sizeof (mac));
  g_output_stream_write_all (out, mac, sizeof (mac), NULL, NULL, NULL);

  self->send_nonce++;
}

void
spotifygtk_ap_session_set_handler (SpotifyApSession *self, ApCommandId cmd,
                                   ApPacketHandler handler, gpointer user_data)
{
  g_hash_table_insert (self->handlers,      GUINT_TO_POINTER (cmd), handler);
  g_hash_table_insert (self->handler_data,  GUINT_TO_POINTER (cmd), user_data);
}

/* ── Handshake ────────────────────────────────────────────────────────────
 * Ported from librespot's core/src/connection/handshake.rs. See
 * research/auth/ for the upstream mapping and confirmed findings.
 *
 * Wire framing during the handshake is NOT the post-handshake
 * [cmd][len][payload]+Shannon-MAC framing used elsewhere in this
 * file -- it's specific to these two messages:
 *   ClientHello:             [0x00,0x04][4-byte BE total_size][protobuf]
 *   APResponseMessage:                  [4-byte BE total_size][protobuf]
 *   ClientResponsePlaintext:            [4-byte BE total_size][protobuf]
 * total_size includes its own header bytes. Nothing is encrypted yet
 * at this stage -- encryption only starts once the Shannon ciphers
 * are keyed at the very end of the handshake.
 */

typedef struct {
  SpotifyApSession *session;
  gchar            *access_token;
} ConnectClosure;

#define HANDSHAKE_MAX_RESPONSE (64 * 1024)  /* sanity bound on the size field */

static void
write_be32 (guint8 *out, guint32 v)
{
  out[0] = (guint8) (v >> 24);
  out[1] = (guint8) (v >> 16);
  out[2] = (guint8) (v >> 8);
  out[3] = (guint8) v;
}

static guint32
read_be32 (const guint8 *in)
{
  return ((guint32) in[0] << 24) | ((guint32) in[1] << 16) |
         ((guint32) in[2] << 8)  |  (guint32) in[3];
}

/* Walks APResponseMessage.challenge.login_crypto_challenge.diffie_hellman
 * to pull out {gs, gs_signature}, or APResponseMessage.login_failed's
 * error_code if the server rejected us outright -- field numbers per
 * research/auth/ (extracted fresh from keyexchange.proto, not from
 * memory). Returns FALSE and sets *error either way on failure. */
static gboolean
parse_ap_response (const guint8 *payload, gsize len,
                   const guint8 **out_gs, gsize *out_gs_len,
                   const guint8 **out_gs_sig, gsize *out_gs_sig_len,
                   GError **error)
{
  const guint8 *challenge_data = NULL; gsize challenge_len = 0;
  if (!pb_find_bytes_field (payload, len, 10, &challenge_data, &challenge_len)) {
    const guint8 *login_failed_data = NULL; gsize login_failed_len = 0;
    if (pb_find_bytes_field (payload, len, 30, &login_failed_data, &login_failed_len)) {
      guint64 error_code = 0;
      pb_find_varint_field (login_failed_data, login_failed_len, 10, &error_code);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "AP rejected handshake, error_code=%" G_GUINT64_FORMAT
                  " (11=PremiumAccountRequired, 2=TryAnotherAP -- see keyexchange.proto ErrorCode)",
                  error_code);
    } else {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "APResponseMessage has neither challenge nor login_failed");
    }
    return FALSE;
  }

  const guint8 *lcc_data = NULL; gsize lcc_len = 0;
  if (!pb_find_bytes_field (challenge_data, challenge_len, 10, &lcc_data, &lcc_len)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "APChallenge missing login_crypto_challenge");
    return FALSE;
  }

  const guint8 *dh_chal_data = NULL; gsize dh_chal_len = 0;
  if (!pb_find_bytes_field (lcc_data, lcc_len, 10, &dh_chal_data, &dh_chal_len)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "LoginCryptoChallengeUnion missing diffie_hellman");
    return FALSE;
  }

  if (!pb_find_bytes_field (dh_chal_data, dh_chal_len, 10, out_gs, out_gs_len)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "LoginCryptoDiffieHellmanChallenge missing gs");
    return FALSE;
  }
  if (!pb_find_bytes_field (dh_chal_data, dh_chal_len, 30, out_gs_sig, out_gs_sig_len)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "LoginCryptoDiffieHellmanChallenge missing gs_signature");
    return FALSE;
  }

  return TRUE;
}

static gboolean
perform_handshake (SpotifyApSession *self, GError **error)
{
  GInputStream  *in  = g_io_stream_get_input_stream  (G_IO_STREAM (self->connection));
  GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (self->connection));

  DhLocalKeys local_keys;
  dh_local_keys_generate (&local_keys);

  guint8 client_nonce[16];
#if HAVE_OPENSSL
  RAND_bytes (client_nonce, sizeof (client_nonce));
#else
  for (gsize i = 0; i < sizeof (client_nonce); i++) client_nonce[i] = (guint8) g_random_int ();
#endif

  /* ── Build ClientHello ── */
  g_autoptr(GByteArray) dh_hello = g_byte_array_new ();
  pb_write_bytes_field  (dh_hello, 10, local_keys.public_key, DH_KEY_BYTES);
  pb_write_varint_field (dh_hello, 20, 1);  /* server_keys_known */

  g_autoptr(GByteArray) crypto_hello_union = g_byte_array_new ();
  pb_write_message_field (crypto_hello_union, 10, dh_hello->data, dh_hello->len);

  g_autoptr(GByteArray) build_info = g_byte_array_new ();
  pb_write_varint_field (build_info, 10, 0);          /* product = PRODUCT_CLIENT */
  pb_write_varint_field (build_info, 20, 0);          /* product_flags = PRODUCT_FLAG_NONE */
  pb_write_varint_field (build_info, 30, 8);          /* platform = PLATFORM_LINUX_X86_64 */
  pb_write_varint_field (build_info, 40, 124200290);  /* version = librespot's SPOTIFY_VERSION */

  g_autoptr(GByteArray) client_hello = g_byte_array_new ();
  pb_write_message_field (client_hello, 10, build_info->data, build_info->len);
  pb_write_varint_field  (client_hello, 30, 0);  /* cryptosuites_supported = [CRYPTO_SUITE_SHANNON] */
  pb_write_message_field (client_hello, 50, crypto_hello_union->data, crypto_hello_union->len);
  pb_write_bytes_field   (client_hello, 60, client_nonce, sizeof (client_nonce));
  guint8 padding_byte = 0x1e;
  pb_write_bytes_field   (client_hello, 70, &padding_byte, 1);

  /* ── Frame + send, accumulating every exchanged byte for compute_keys() ── */
  g_autoptr(GByteArray) accumulator = g_byte_array_new ();

  guint8 hello_prefix[6];
  hello_prefix[0] = 0x00; hello_prefix[1] = 0x04;
  write_be32 (hello_prefix + 2, 2 + 4 + client_hello->len);
  g_byte_array_append (accumulator, hello_prefix, sizeof (hello_prefix));
  g_byte_array_append (accumulator, client_hello->data, client_hello->len);

  if (!g_output_stream_write_all (out, accumulator->data, accumulator->len, NULL, NULL, error))
    return FALSE;

  /* ── Receive APResponseMessage ── */
  guint8 resp_size_be[4];
  if (!g_input_stream_read_all (in, resp_size_be, sizeof (resp_size_be), NULL, NULL, error))
    return FALSE;

  guint32 resp_total = read_be32 (resp_size_be);
  if (resp_total < 4 || resp_total > HANDSHAKE_MAX_RESPONSE) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "implausible handshake response size: %u", resp_total);
    return FALSE;
  }
  guint32 resp_payload_len = resp_total - 4;
  g_autofree guint8 *resp_payload = g_malloc (resp_payload_len);
  if (!g_input_stream_read_all (in, resp_payload, resp_payload_len, NULL, NULL, error))
    return FALSE;

  g_byte_array_append (accumulator, resp_size_be, sizeof (resp_size_be));
  g_byte_array_append (accumulator, resp_payload, resp_payload_len);

  const guint8 *gs = NULL, *gs_sig = NULL;
  gsize gs_len = 0, gs_sig_len = 0;
  if (!parse_ap_response (resp_payload, resp_payload_len, &gs, &gs_len, &gs_sig, &gs_sig_len, error))
    return FALSE;

  if (!hs_verify_server_signature (gs, gs_len, gs_sig, gs_sig_len)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "AP server signature verification failed -- possible MITM, or "
                        "AP_SERVER_KEY in handshake_constants.h is stale");
    return FALSE;
  }

  guint8 shared_secret[DH_KEY_BYTES];
  if (!dh_compute_shared_secret (&local_keys, gs, gs_len, shared_secret, sizeof (shared_secret))) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "DH shared secret computation failed");
    return FALSE;
  }

  guint8 challenge_response[HS_CHALLENGE_LEN];
  guint8 send_key[HS_SEND_KEY_LEN], recv_key[HS_RECV_KEY_LEN];
  if (!hs_compute_keys (shared_secret, sizeof (shared_secret),
                        accumulator->data, accumulator->len,
                        challenge_response, send_key, recv_key)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "HMAC key derivation failed");
    return FALSE;
  }

  /* ── Build + send ClientResponsePlaintext ──
   * pow_response (field 20) and crypto_response (field 30) are sent
   * as empty embedded messages -- librespot does the same
   * (mut_or_insert_default with no sub-fields set), matching what
   * the server actually expects here rather than real PoW/crypto
   * challenge answers. */
  g_autoptr(GByteArray) dh_response = g_byte_array_new ();
  pb_write_bytes_field (dh_response, 10, challenge_response, sizeof (challenge_response));

  g_autoptr(GByteArray) crypto_response_union = g_byte_array_new ();
  pb_write_message_field (crypto_response_union, 10, dh_response->data, dh_response->len);

  g_autoptr(GByteArray) client_response = g_byte_array_new ();
  pb_write_message_field (client_response, 10, crypto_response_union->data, crypto_response_union->len);
  pb_write_message_field (client_response, 20, NULL, 0);  /* pow_response, empty */
  pb_write_message_field (client_response, 30, NULL, 0);  /* crypto_response, empty */

  guint8 resp2_size_be[4];
  write_be32 (resp2_size_be, 4 + client_response->len);

  if (!g_output_stream_write_all (out, resp2_size_be, sizeof (resp2_size_be), NULL, NULL, error))
    return FALSE;
  if (!g_output_stream_write_all (out, client_response->data, client_response->len, NULL, NULL, error))
    return FALSE;

  /* ── Encryption starts now ── */
  shannon_key_setup (&self->send_cipher, send_key, sizeof (send_key));
  shannon_key_setup (&self->recv_cipher, recv_key, sizeof (recv_key));
  self->send_nonce = 0;
  self->recv_nonce = 0;
  shannon_nonce_u32 (&self->send_cipher, self->send_nonce);
  shannon_nonce_u32 (&self->recv_cipher, self->recv_nonce);

  self->connected = TRUE;
  return TRUE;
}

static void
on_tcp_connected (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data;
  ConnectClosure *cl = g_task_get_task_data (task);
  g_autoptr(GError) err = NULL;

  GSocketConnection *conn = g_socket_client_connect_to_host_finish (
    G_SOCKET_CLIENT (source), result, &err);

  if (!conn) {
    g_task_return_error (task, err ? g_steal_pointer (&err) :
      g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "AP connect failed"));
    return;
  }

  cl->session->connection = conn;

  g_autoptr(GError) hs_err = NULL;
  if (!perform_handshake (cl->session, &hs_err)) {
    g_warning ("ap.c: handshake failed: %s", hs_err ? hs_err->message : "unknown error");
    g_task_return_error (task, g_steal_pointer (&hs_err));
    return;
  }

  g_message ("ap.c: handshake completed, encrypted channel established");
  g_task_return_boolean (task, TRUE);
}

static void
on_host_resolved (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data;
  ConnectClosure *cl = g_task_get_task_data (task);

  g_autofree gchar *hostport = g_task_propagate_pointer (G_TASK (result), NULL);
  if (!hostport) hostport = g_strdup_printf ("%s:%d", AP_FALLBACK_HOST, AP_FALLBACK_PORT);

  gchar **parts = g_strsplit (hostport, ":", 2);
  const gchar *host = parts[0];
  guint16 port = parts[1] ? (guint16) g_ascii_strtoull (parts[1], NULL, 10) : AP_FALLBACK_PORT;

  cl->session->client = g_socket_client_new ();
  g_socket_client_connect_to_host_async (cl->session->client, host, port, NULL,
                                         on_tcp_connected, task);
  g_strfreev (parts);
  (void) source;
}

static void
connect_closure_free (ConnectClosure *cl)
{
  g_free (cl->access_token);
  g_free (cl);
}

void
spotifygtk_ap_session_connect (SpotifyApSession *self, const gchar *access_token,
                               GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_AP_SESSION (self));

  GTask *task = g_task_new (self, NULL, callback, user_data);
  ConnectClosure *cl = g_new0 (ConnectClosure, 1);
  cl->session = self;
  cl->access_token = g_strdup (access_token);
  g_task_set_task_data (task, cl, (GDestroyNotify) connect_closure_free);

  resolve_ap_host (on_host_resolved, task);
}

gboolean
spotifygtk_ap_session_connect_finish (SpotifyApSession *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

void
spotifygtk_ap_session_disconnect (SpotifyApSession *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_AP_SESSION (self));
  if (self->connection) {
    g_io_stream_close (G_IO_STREAM (self->connection), NULL, NULL);
    g_clear_object (&self->connection);
  }
  self->connected = FALSE;
}

static void
spotifygtk_ap_session_dispose (GObject *object)
{
  SpotifyApSession *self = SPOTIFYGTK_AP_SESSION (object);
  spotifygtk_ap_session_disconnect (self);
  g_clear_object (&self->client);
  g_clear_pointer (&self->handlers, g_hash_table_unref);
  g_clear_pointer (&self->handler_data, g_hash_table_unref);
  G_OBJECT_CLASS (spotifygtk_ap_session_parent_class)->dispose (object);
}

static void
spotifygtk_ap_session_class_init (SpotifyApSessionClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_ap_session_dispose;
}

static void
spotifygtk_ap_session_init (SpotifyApSession *self)
{
  self->handlers     = g_hash_table_new (NULL, NULL);
  self->handler_data = g_hash_table_new (NULL, NULL);
}

SpotifyApSession *
spotifygtk_ap_session_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_AP_SESSION, NULL);
}

/*
 * ap.c — Access Point connection implementation.
 *
 * STATUS: SRV resolution, TCP connect, packet framing, the
 * Diffie-Hellman handshake, the post-handshake receive loop, and
 * login are all real now, and the handshake has been confirmed
 * working against a live Spotify server (DH exchange, RSA signature
 * verification, and HMAC key derivation all checked out). Login
 * itself is implemented and sends correctly, but hasn't yet produced
 * a confirmed APWelcome — see research/auth/ for the current status
 * of that specific gap.
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

enum {
  SIG_DISCONNECTED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

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

  /* librespot's ApCodec re-nonces from the per-direction counter
   * before every single packet (see codec.rs) -- this was previously
   * missing here: send_nonce incremented but was never fed back into
   * the cipher, so every packet after the very first (which inherited
   * the handshake's initial nonce-0 state) would have used stale
   * keystream/MAC state and corrupted the connection. */
  shannon_nonce_u32 (&self->send_cipher, self->send_nonce);

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

/* ── Post-handshake receive loop ─────────────────────────────────────────────
 * Ported against codec.rs's decode path: read a 3-byte header, Shannon-
 * decrypt it (re-nonced from recv_nonce first, same as the send side),
 * pull cmd+len out, read that many payload bytes + the trailing 4-byte
 * MAC, decrypt the payload (continuing the same per-packet keystream
 * the header started), then verify the MAC before dispatching to a
 * handler.
 *
 * The header read is async (so this doesn't block waiting for the
 * *next* packet to arrive at all); the payload+MAC read is a
 * synchronous read_all once the header tells us how many bytes to
 * expect, since by that point the data is either already buffered or
 * arriving imminently as part of the same logical packet -- avoids
 * threading a second GAsyncResult through for what's normally a tiny,
 * already-in-flight read. */

static void start_next_read (SpotifyApSession *self);

static void
on_header_read (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyApSession *self = user_data;
  g_autoptr(GError) err = NULL;

  GBytes *header_bytes = g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &err);
  if (!header_bytes || g_bytes_get_size (header_bytes) < 3) {
    g_autoptr(GError) close_err = NULL;
    if (err) {
      g_warning ("ap.c: receive loop header read failed: %s", err->message);
      close_err = g_error_copy (err);
    } else {
      g_message ("ap.c: AP connection closed by remote end");
      close_err = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CLOSED,
                                       "AP connection closed by remote end");
    }
    g_clear_pointer (&header_bytes, g_bytes_unref);
    spotifygtk_ap_session_disconnect (self);
    g_signal_emit (self, signals[SIG_DISCONNECTED], 0, close_err);
    g_object_unref (self);  /* matches the ref taken in start_next_read() */
    return;  /* loop stops -- no automatic reconnect at this layer */
  }

  shannon_nonce_u32 (&self->recv_cipher, self->recv_nonce);

  gsize hlen = 0;
  const guint8 *hdata = g_bytes_get_data (header_bytes, &hlen);
  guint8 header[3];
  memcpy (header, hdata, 3);
  g_bytes_unref (header_bytes);

  shannon_decrypt (&self->recv_cipher, header, 3);

  ApCommandId cmd     = (ApCommandId) header[0];
  guint16     pay_len = (guint16) ((header[1] << 8) | header[2]);

  GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (self->connection));

  g_autofree guint8 *payload_and_mac = g_malloc ((gsize) pay_len + 4);
  if (!g_input_stream_read_all (in, payload_and_mac, (gsize) pay_len + 4, NULL, NULL, &err)) {
    g_warning ("ap.c: receive loop payload read failed: %s", err ? err->message : "unknown");
    spotifygtk_ap_session_disconnect (self);
    g_autoptr(GError) payload_err = err ? g_error_copy (err) :
      g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "receive loop payload read failed");
    g_signal_emit (self, signals[SIG_DISCONNECTED], 0, payload_err);
    g_object_unref (self);
    return;
  }

  guint8 *payload_ptr = payload_and_mac;
  if (pay_len > 0)
    shannon_decrypt (&self->recv_cipher, payload_ptr, pay_len);

  const guint8 *expected_mac = payload_and_mac + pay_len;
  if (!shannon_check_mac (&self->recv_cipher, expected_mac, 4)) {
    g_warning ("ap.c: MAC verification failed on incoming packet (cmd=0x%02x) -- "
              "dropping connection, this should never happen on a correct wire", cmd);
    spotifygtk_ap_session_disconnect (self);
    g_autoptr(GError) mac_err = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "MAC verification failed on incoming packet (cmd=0x%02x)", cmd);
    g_signal_emit (self, signals[SIG_DISCONNECTED], 0, mac_err);
    g_object_unref (self);
    return;
  }

  ApPacketHandler handler = g_hash_table_lookup (self->handlers, GUINT_TO_POINTER (cmd));
  if (handler) {
    gpointer hdata2 = g_hash_table_lookup (self->handler_data, GUINT_TO_POINTER (cmd));
    handler (self, cmd, payload_ptr, pay_len, hdata2);
  } else {
    g_message ("ap.c: no handler registered for incoming cmd=0x%02x (%u bytes), ignoring",
              cmd, pay_len);
  }

  self->recv_nonce++;

  /* start_next_read() below takes its own fresh ref for the next
   * iteration; drop the one this callback was holding regardless of
   * which order these two happen in (single-threaded GMainLoop
   * context, so there's no window where the refcount could hit zero
   * between them even if it did matter). */
  start_next_read (self);
  g_object_unref (self);
}

static void
start_next_read (SpotifyApSession *self)
{
  if (!self->connected || !self->connection) return;

  /* Ref held across the async gap -- released in on_header_read() on
   * every exit path. Without this, disposing the session while a
   * read is in flight would leave the callback firing on a freed
   * object. */
  g_object_ref (self);

  GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (self->connection));
  g_input_stream_read_bytes_async (in, 3, G_PRIORITY_DEFAULT, NULL, on_header_read, self);
}

void
spotifygtk_ap_session_start_receiving (SpotifyApSession *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_AP_SESSION (self));
  start_next_read (self);
}

/* ── Login ────────────────────────────────────────────────────────────────── */

typedef struct {
  SpotifyApSession *session;
  ApLoginCallback   callback;
  gpointer          user_data;
  gulong            disconnect_handler_id;
} LoginClosure;

static void
login_closure_finish (LoginClosure *lc, SpotifyApSession *session)
{
  /* Common cleanup shared by all three ways a pending login can
   * resolve (APWelcome, AuthFailure, or the connection dropping
   * before either arrives) -- unregister both packet handlers and
   * the "disconnected" listener, then free the closure. Centralized
   * here so a future fourth resolution path can't forget a step. */
  spotifygtk_ap_session_set_handler (session, AP_CMD_APWELCOME, NULL, NULL);
  spotifygtk_ap_session_set_handler (session, AP_CMD_AUTH_FAILURE, NULL, NULL);
  if (lc->disconnect_handler_id)
    g_signal_handler_disconnect (session, lc->disconnect_handler_id);
  g_free (lc);
}

static void
on_apwelcome (SpotifyApSession *session, ApCommandId cmd,
             const guint8 *payload, gsize len, gpointer user_data)
{
  (void) cmd;
  LoginClosure *lc = user_data;

  const guint8 *username_data = NULL; gsize username_len = 0;
  /* APWelcome.canonical_username, field 0x14 per authentication.proto */
  pb_find_bytes_field (payload, len, 0x14, &username_data, &username_len);

  g_autofree gchar *username = username_data
    ? g_strndup ((const gchar *) username_data, username_len) : NULL;

  g_message ("ap.c: login succeeded%s%s", username ? " as " : "", username ? username : "");

  if (lc->callback) lc->callback (TRUE, username, NULL, lc->user_data);

  login_closure_finish (lc, session);
}

static void
on_auth_failure (SpotifyApSession *session, ApCommandId cmd,
                 const guint8 *payload, gsize len, gpointer user_data)
{
  (void) cmd;
  LoginClosure *lc = user_data;

  guint64 error_code = 0;
  pb_find_varint_field (payload, len, 10, &error_code);  /* APLoginFailed.error_code */

  /* Values per keyexchange.proto's ErrorCode enum, see research/auth/. */
  const gchar *desc = "unknown error";
  switch (error_code) {
    case 0x0:  desc = "ProtocolError"; break;
    case 0x2:  desc = "TryAnotherAP"; break;
    case 0x5:  desc = "BadConnectionId"; break;
    case 0x9:  desc = "TravelRestriction"; break;
    case 0xb:  desc = "PremiumAccountRequired"; break;
    case 0xc:  desc = "BadCredentials"; break;
    case 0xd:  desc = "CouldNotValidateCredentials"; break;
    case 0xe:  desc = "AccountExists"; break;
    case 0xf:  desc = "ExtraVerificationRequired"; break;
    case 0x10: desc = "InvalidAppKey"; break;
    case 0x11: desc = "ApplicationBanned"; break;
  }

  GError *err = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                             "AP login failed: %s (code 0x%" G_GINT64_MODIFIER "x)",
                             desc, error_code);
  g_warning ("ap.c: %s", err->message);

  if (lc->callback) lc->callback (FALSE, NULL, err, lc->user_data);
  g_error_free (err);

  login_closure_finish (lc, session);
}

static void
on_session_disconnected_during_login (SpotifyApSession *session, GError *error, gpointer user_data)
{
  LoginClosure *lc = user_data;

  /* The connection dropped (closed, read failure, or MAC failure)
   * before either APWelcome or AuthFailure arrived. Spotify's AP
   * service doesn't always send a structured AuthFailure for a
   * rejected login (a clearly invalid token, for instance, can just
   * get the raw connection closed) -- this is what lets that case
   * fail fast as a login failure, instead of the caller waiting out
   * its own timeout for a structured response that was never coming. */
  GError *login_err = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "AP connection dropped before login completed%s%s",
                                   error ? ": " : "", error ? error->message : "");
  g_warning ("ap.c: %s", login_err->message);

  if (lc->callback) lc->callback (FALSE, NULL, login_err, lc->user_data);
  g_error_free (login_err);

  /* The "disconnected" signal already fired (we're a handler for it
   * right now) and the session already called disconnect() before
   * emitting -- don't try to disconnect our own handler ID from
   * inside its own emission, that's a g_signal_handler_disconnect()
   * misuse. login_closure_finish() below skips that step since
   * lc->disconnect_handler_id gets zeroed first. */
  lc->disconnect_handler_id = 0;
  login_closure_finish (lc, session);
}

void
spotifygtk_ap_session_login (SpotifyApSession *self, const gchar *spotify_username,
                             const gchar *oauth_access_token,
                             ApLoginCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_AP_SESSION (self));

  if (!self->connected) {
    GError *err = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "login attempted before handshake completed");
    if (callback) callback (FALSE, NULL, err, user_data);
    g_error_free (err);
    return;
  }

  /* LoginCredentials: username (field 0x0a), typ (0x14, enum
   * AuthenticationType -- AUTHENTICATION_SPOTIFY_TOKEN = 8 per
   * authentication.proto), auth_data (0x1e, the raw token bytes). */
  g_autoptr(GByteArray) login_credentials = g_byte_array_new ();
  if (spotify_username && *spotify_username)
    pb_write_bytes_field (login_credentials, 0x0a,
                          (const guint8 *) spotify_username, strlen (spotify_username));
  pb_write_varint_field (login_credentials, 0x14, 8);  /* AUTHENTICATION_SPOTIFY_TOKEN */
  pb_write_bytes_field  (login_credentials, 0x1e,
                        (const guint8 *) oauth_access_token, strlen (oauth_access_token));

  /* SystemInfo (authentication.proto): cpu_family (0x0a, REQUIRED)
   * and os (0x3c, REQUIRED) -- a previous version of this function
   * sent SystemInfo entirely empty, which is invalid proto2 (both
   * fields lack defaults) and is the most likely reason a live login
   * attempt got a fast, silent connection close instead of a
   * structured AuthFailure: a strict server-side parser is entitled
   * to hard-reject a message missing required fields rather than
   * respond gracefully. Values below match librespot's own
   * connection/mod.rs construction for a Linux x86_64 build:
   * CPU_X86_64 = 0x2, OS_LINUX = 0x5 (both per authentication.proto's
   * enums, re-verified against the source, not reused from memory of
   * an earlier pass in this file). system_information_string (0x5a)
   * and device_id (0x64) are optional but included for parity with
   * librespot's real client, which always sends them. */
  g_autoptr(GByteArray) system_info = g_byte_array_new ();
  pb_write_varint_field (system_info, 0x0a, 0x2);  /* cpu_family = CPU_X86_64 */
  pb_write_varint_field (system_info, 0x3c, 0x5);  /* os = OS_LINUX */
  {
    g_autofree gchar *sysinfo_str = g_strdup_printf ("spotify-native-%s", APP_VERSION);
    pb_write_bytes_field (system_info, 0x5a, (const guint8 *) sysinfo_str, strlen (sysinfo_str));
  }
  pb_write_bytes_field (system_info, 0x64, (const guint8 *) "spotifygtk-native", 17);

  /* ClientResponseEncrypted: login_credentials (0x0a, required),
   * system_info (0x32, required -- NOT 0x14, which is actually
   * account_creation on this message; a prior version of this
   * function used 0x14 by mistake, meaning even a correctly-built
   * SystemInfo would have landed in the wrong field entirely).
   * version_string (0x46, optional) included for parity with
   * librespot's real client. */
  g_autoptr(GByteArray) client_response = g_byte_array_new ();
  pb_write_message_field (client_response, 0x0a, login_credentials->data, login_credentials->len);
  pb_write_message_field (client_response, 0x32, system_info->data, system_info->len);
  {
    const gchar *version_str = "spotify-native " APP_VERSION;
    pb_write_bytes_field (client_response, 0x46, (const guint8 *) version_str, strlen (version_str));
  }

  LoginClosure *lc = g_new0 (LoginClosure, 1);
  lc->session   = self;
  lc->callback  = callback;
  lc->user_data = user_data;

  spotifygtk_ap_session_set_handler (self, AP_CMD_APWELCOME,    on_apwelcome,    lc);
  spotifygtk_ap_session_set_handler (self, AP_CMD_AUTH_FAILURE, on_auth_failure, lc);
  lc->disconnect_handler_id = g_signal_connect (self, "disconnected",
                                                G_CALLBACK (on_session_disconnected_during_login), lc);

  spotifygtk_ap_session_send (self, AP_CMD_LOGIN, client_response->data, client_response->len);
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

  signals[SIG_DISCONNECTED] =
    g_signal_new ("disconnected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
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

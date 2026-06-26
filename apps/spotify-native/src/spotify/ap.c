/*
 * ap.c — Access Point connection implementation.
 *
 * STATUS: SRV resolution, TCP connect, and packet framing are real
 * and functional. The Diffie-Hellman key exchange that establishes
 * the Shannon session keys is scaffolded but marked TODO — it depends
 * on a real big-integer modexp (we'd pull this from OpenSSL's BIGNUM
 * API, already an optional dependency) and on shannon.c's round
 * function being filled in first. Wiring DH against a stub cipher
 * would just produce a connection that handshakes successfully and
 * then fails silently on the first real packet, which is worse than
 * leaving it explicit.
 */

#include "config.h"
#include "ap.h"

#include <gio/gio.h>
#include <string.h>

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
 * TODO: Diffie-Hellman key exchange (send ClientHello with our DH public
 * key + nonce, receive APResponse with server DH public key + signed
 * Diffie-Hellman params + challenge, compute shared secret via modexp,
 * derive Shannon send/recv keys via HMAC-SHA1). Needs BIGNUM (OpenSSL)
 * for modexp and shannon.c's round function completed before the
 * resulting keys would actually work against a live server. */

typedef struct {
  SpotifyApSession *session;
  gchar            *access_token;
} ConnectClosure;

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

  /* TODO: perform DH handshake here. For now we mark "connected" at the
   * TCP layer only — Shannon-encrypted traffic will not work until the
   * handshake + cipher round function are both implemented. */
  g_warning ("ap.c: TCP connected, but DH handshake is not yet implemented — "
            "no encrypted traffic will succeed past this point");

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

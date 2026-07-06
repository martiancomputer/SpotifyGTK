/*
 * clienttoken.c — client-token exchange implementation.
 * See clienttoken.h for scope/rationale.
 *
 * client_version is librespot's own SPOTIFY_SEMANTIC_VERSION
 * ("1.2.52.442", core/src/version.rs) -- reused verbatim rather than
 * invented, since this identifies the client to a server that may
 * validate it against known-good version strings. Skipped:
 * connectivity_sdk_data's platform/OS/kernel-version details, which
 * librespot's real client does populate -- a reasonable
 * simplification given client-token is tolerated as optional even
 * when entirely absent (confirmed in spclient.rs), so a minimal-but-
 * present request should be more than sufficient.
 */

#include "config.h"
#include "clienttoken.h"
#include "protobuf_min.h"

#include <libsoup/soup.h>
#include <string.h>

#define SPOTIFY_SEMANTIC_VERSION "1.2.52.442"

struct _SpotifyClientToken {
  GObject      parent_instance;
  SoupSession *session;
};

G_DEFINE_FINAL_TYPE (SpotifyClientToken, spotifygtk_client_token, G_TYPE_OBJECT)

typedef struct {
  ClientTokenCallback callback;
  gpointer            user_data;
} RequestClosure;

static void
on_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RequestClosure    *cl  = user_data;
  g_autoptr(GError)  err = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  if (!bytes) {
    g_warning ("clienttoken: request failed: %s -- proceeding without a client-token",
              err ? err->message : "unknown");
    if (cl->callback) cl->callback (NULL, cl->user_data);
    g_free (cl);
    return;
  }

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);

  /* ClientTokenResponse: response_type (field 1, varint), oneof
   * granted_token (field 2, embedded GrantedTokenResponse) or
   * challenges (field 3). GrantedTokenResponse.token is field 1
   * (string). All per clienttoken_http.proto, proto3 field numbers. */
  const guint8 *granted_data = NULL; gsize granted_len = 0;
  if (pb_find_bytes_field (data, len, 2, &granted_data, &granted_len)) {
    const guint8 *token_data = NULL; gsize token_len = 0;
    if (pb_find_bytes_field (granted_data, granted_len, 1, &token_data, &token_len)) {
      g_autofree gchar *token = g_strndup ((const gchar *) token_data, token_len);
      g_message ("clienttoken: obtained a client-token");
      if (cl->callback) cl->callback (token, cl->user_data);
      g_bytes_unref (bytes);
      g_free (cl);
      return;
    }
  }

  /* Field 3 (challenges) or anything else unparseable -- treat as a
   * soft failure, matching librespot's tolerance for this endpoint
   * not being strictly required. Solving a HashCash/JS-eval/HMAC
   * challenge is real, separate work not implemented here. */
  g_message ("clienttoken: no granted_token in response (challenge or unknown shape) -- "
            "proceeding without a client-token");
  if (cl->callback) cl->callback (NULL, cl->user_data);
  g_bytes_unref (bytes);
  g_free (cl);
}

void
spotifygtk_client_token_request (SpotifyClientToken *self, const gchar *client_id,
                                 ClientTokenCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_CLIENT_TOKEN (self));

  /* ClientDataRequest: client_version (field 1, string), client_id
   * (field 2, string). */
  g_autoptr(GByteArray) client_data = g_byte_array_new ();
  pb_write_bytes_field (client_data, 1, (const guint8 *) SPOTIFY_SEMANTIC_VERSION,
                        strlen (SPOTIFY_SEMANTIC_VERSION));
  pb_write_bytes_field (client_data, 2, (const guint8 *) client_id, strlen (client_id));

  /* ClientTokenRequest: request_type (field 1, varint,
   * REQUEST_CLIENT_DATA_REQUEST = 1), client_data (field 2, embedded,
   * oneof "request"). */
  g_autoptr(GByteArray) request = g_byte_array_new ();
  pb_write_varint_field  (request, 1, 1);
  pb_write_message_field (request, 2, client_data->data, client_data->len);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, CLIENTTOKEN_URL);
  soup_message_headers_replace (soup_message_get_request_headers (msg),
                                "Accept", "application/x-protobuf");

  GBytes *body = g_bytes_new (request->data, request->len);
  soup_message_set_request_body_from_bytes (msg, "application/x-protobuf", body);
  g_bytes_unref (body);

  RequestClosure *cl = g_new0 (RequestClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, NULL,
                                    on_response, cl);
  g_object_unref (msg);
}

static void
spotifygtk_client_token_dispose (GObject *object)
{
  SpotifyClientToken *self = SPOTIFYGTK_CLIENT_TOKEN (object);
  g_clear_object (&self->session);
  G_OBJECT_CLASS (spotifygtk_client_token_parent_class)->dispose (object);
}

static void
spotifygtk_client_token_class_init (SpotifyClientTokenClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_client_token_dispose;
}

static void
spotifygtk_client_token_init (SpotifyClientToken *self)
{
  self->session = soup_session_new_with_options ("user-agent", "spotify-native/" APP_VERSION, NULL);
}

SpotifyClientToken *
spotifygtk_client_token_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_CLIENT_TOKEN, NULL);
}

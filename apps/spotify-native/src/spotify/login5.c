/*
 * login5.c — login5 exchange implementation.
 * See login5.h for scope/rationale.
 *
 * Confirmed against core/src/login5.rs: a client-token is NOT
 * optional for this specific request (the reference code propagates
 * failure with `?` on client_token() before ever building the
 * LoginRequest), unlike some other spclient endpoints that tolerate
 * its absence. Callers of spotifygtk_login5_auth_token() should
 * fetch a client-token first (see clienttoken.h) and pass it in --
 * this file does not fetch one itself, keeping the two concerns
 * separate and independently testable.
 */

#include "config.h"
#include "login5.h"
#include "protobuf_min.h"

#include <libsoup/soup.h>
#include <string.h>

struct _SpotifyLogin5 {
  GObject      parent_instance;
  SoupSession *session;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (SpotifyLogin5, spotifygtk_login5, G_TYPE_OBJECT)

typedef struct {
  Login5Callback callback;
  gpointer       user_data;
} RequestClosure;

static void
on_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RequestClosure    *cl  = user_data;
  g_autoptr(GError)  err = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  if (!bytes) {
    if (cl->callback) cl->callback (NULL, 0, err, cl->user_data);
    g_free (cl);
    return;
  }

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);

  /* LoginResponse's oneof "response": ok=1, error=2, challenges=3.
   * LoginOk: username=1(string), access_token=2(string),
   * stored_credential=3(bytes), access_token_expires_in=4(int32).
   * All per login5.proto, proto3 field numbers. */
  const guint8 *ok_data = NULL; gsize ok_len = 0;
  if (pb_find_bytes_field (data, len, 1, &ok_data, &ok_len)) {
    const guint8 *token_data = NULL; gsize token_len = 0;
    if (pb_find_bytes_field (ok_data, ok_len, 2, &token_data, &token_len)) {
      g_autofree gchar *token = g_strndup ((const gchar *) token_data, token_len);
      guint64 expires_in = 3600;  /* fallback if the field is somehow absent */
      pb_find_varint_field (ok_data, ok_len, 4, &expires_in);
      g_message ("login5: obtained an access token (expires in %" G_GUINT64_FORMAT "s)", expires_in);
      if (cl->callback) cl->callback (token, (gint32) expires_in, NULL, cl->user_data);
      g_bytes_unref (bytes);
      g_free (cl);
      return;
    }
  }

  /* Field 2 (error) or 3 (challenges, e.g. a hashcash/code challenge
   * we don't solve) -- surface as a real failure, unlike
   * clienttoken.c's tolerant handling, since login5 succeeding is
   * genuinely required for anything downstream (spclient calls). */
  GError *login_err = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
    "login5 did not return LoginOk (error or unsolved challenge response)");
  g_warning ("login5: %s", login_err->message);
  if (cl->callback) cl->callback (NULL, 0, login_err, cl->user_data);
  g_error_free (login_err);
  g_bytes_unref (bytes);
  g_free (cl);
}

void
spotifygtk_login5_auth_token (SpotifyLogin5 *self, const gchar *client_id, const gchar *device_id,
                              const gchar *username, const guint8 *reusable_creds,
                              gsize reusable_creds_len, guint64 reusable_creds_type,
                              const gchar *client_token, Login5Callback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_LOGIN5 (self));

  if (!client_token || !*client_token) {
    GError *err = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
      "login5 requires a client-token, none was provided");
    g_warning ("login5: %s", err->message);
    if (callback) callback (NULL, 0, err, user_data);
    g_error_free (err);
    return;
  }

  /* ClientInfo: client_id (field 1, string), device_id (field 2, string). */
  g_autoptr(GByteArray) client_info = g_byte_array_new ();
  pb_write_bytes_field (client_info, 1, (const guint8 *) client_id, strlen (client_id));
  pb_write_bytes_field (client_info, 2, (const guint8 *) device_id, strlen (device_id));

  /* credentials.StoredCredential: username (field 1, string, optional
   * -- omitted if NULL, same reasoning as the AP login's
   * LoginCredentials.username), data (field 2, bytes -- the
   * reusable_auth_credentials blob from APWelcome, NOT the original
   * OAuth token). */
  g_autoptr(GByteArray) stored_cred = g_byte_array_new ();
  if (username && *username)
    pb_write_bytes_field (stored_cred, 1, (const guint8 *) username, strlen (username));
  pb_write_bytes_field (stored_cred, 2, reusable_creds, reusable_creds_len);
  (void) reusable_creds_type;  /* not part of StoredCredential's own wire shape; kept in the
                                * function signature for callers/callsites that may want to
                                * branch on it later (e.g. if a non-token credential type ever
                                * needs different handling here) */

  /* LoginRequest: client_info (field 1, embedded), oneof login_method
   * -> stored_credential (field 100, embedded StoredCredential). */
  g_autoptr(GByteArray) login_request = g_byte_array_new ();
  pb_write_message_field (login_request, 1,   client_info->data, client_info->len);
  pb_write_message_field (login_request, 100, stored_cred->data, stored_cred->len);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, LOGIN5_URL);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  soup_message_headers_replace (headers, "Accept", "application/x-protobuf");
  soup_message_headers_replace (headers, "Client-Token", client_token);

  GBytes *body = g_bytes_new (login_request->data, login_request->len);
  soup_message_set_request_body_from_bytes (msg, "application/x-protobuf", body);
  g_bytes_unref (body);

  RequestClosure *cl = g_new0 (RequestClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, self->cancellable,
                                    on_response, cl);
  g_object_unref (msg);
}

static void
spotifygtk_login5_dispose (GObject *object)
{
  SpotifyLogin5 *self = SPOTIFYGTK_LOGIN5 (object);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
  G_OBJECT_CLASS (spotifygtk_login5_parent_class)->dispose (object);
}

static void
spotifygtk_login5_class_init (SpotifyLogin5Class *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_login5_dispose;
}

static void
spotifygtk_login5_init (SpotifyLogin5 *self)
{
  self->session = soup_session_new_with_options ("user-agent", "spotify-native/" APP_VERSION, NULL);
}

SpotifyLogin5 *
spotifygtk_login5_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_LOGIN5, NULL);
}

void spotifygtk_login5_set_cancellable (SpotifyLogin5 *self, GCancellable *cancellable)
{ g_set_object (&self->cancellable, cancellable); }

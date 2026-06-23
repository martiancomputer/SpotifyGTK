/*
 * auth.c — Spotify OAuth 2.0 PKCE flow for desktop apps.
 *
 * Flow:
 *   1. Generate cryptographically-random code_verifier (64 bytes -> base64url).
 *   2. Derive code_challenge = BASE64URL(SHA-256(code_verifier)).
 *   3. Spin up a local SoupServer on the redirect port to catch the callback.
 *   4. Open the system browser at accounts.spotify.com/authorize.
 *   5. Exchange the received `code` for access + refresh tokens.
 *   6. Store tokens via libsecret (or ~/.config fallback).
 *   7. Emit ::completed(TRUE) on success, ::completed(FALSE) on error.
 */

#include "config.h"
#include "auth.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#ifdef HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

#define REDIRECT_PORT      8888
#define REDIRECT_URI       "http://127.0.0.1:8888/callback"
#define SPOTIFY_AUTH_URL   "https://accounts.spotify.com/authorize"
#define SPOTIFY_TOKEN_URL  "https://accounts.spotify.com/api/token"

struct _SpotifyAuth {
  GObject parent_instance;

  gchar *code_verifier;
  gchar *state_nonce;

  gchar *access_token;
  gchar *refresh_token;
  gint64 expires_at;

  SoupSession *session;
  SoupServer  *redirect_server;
};

enum { SIG_COMPLETED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (SpotifyAuth, spotifygtk_auth, G_TYPE_OBJECT)

static gchar *
base64url_encode (const guchar *data, gsize len)
{
  gchar *b64 = g_base64_encode (data, len);
  for (gchar *p = b64; *p; p++) {
    if      (*p == '+') *p = '-';
    else if (*p == '/') *p = '_';
    else if (*p == '=') { *p = '\0'; break; }
  }
  return b64;
}

static gchar *
generate_random_base64url (gsize byte_count)
{
  guchar *buf = g_malloc (byte_count);
  GRand  *rand = g_rand_new ();
  for (gsize i = 0; i < byte_count; i++)
    buf[i] = (guchar) g_rand_int_range (rand, 0, 256);
  g_rand_free (rand);
  gchar *enc = base64url_encode (buf, byte_count);
  g_free (buf);
  return enc;
}

static gchar *
sha256_base64url (const gchar *input)
{
  GChecksum *cs  = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (cs, (const guchar *) input, -1);

  gsize   digest_len = 32;
  guchar  digest[32];
  g_checksum_get_digest (cs, digest, &digest_len);
  g_checksum_free (cs);

  return base64url_encode (digest, digest_len);
}

static void
store_tokens (SpotifyAuth *self)
{
#ifdef HAVE_LIBSECRET
  g_autoptr(GError) err = NULL;
  g_autofree gchar *blob = g_strdup_printf ("%s\n%s\n%" G_GINT64_FORMAT,
                                            self->access_token, self->refresh_token,
                                            self->expires_at);
  static const SecretSchema schema = {
    "com.github.spotifygtk.SpotifyGTK", SECRET_SCHEMA_NONE,
    { { "type", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } }
  };
  secret_password_store_sync (&schema, SECRET_COLLECTION_DEFAULT,
                              "SpotifyGTK tokens", blob, NULL, &err,
                              "type", "tokens", NULL);
  if (err)
    g_warning ("libsecret store failed: %s", err->message);
#else
  g_autofree gchar *path = g_build_filename (g_get_user_config_dir (),
                                             "spotifygtk", "tokens", NULL);
  g_autofree gchar *dir  = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_autofree gchar *data = g_strdup_printf ("%s\n%s\n%" G_GINT64_FORMAT "\n",
                                            self->access_token,
                                            self->refresh_token,
                                            self->expires_at);
  g_file_set_contents (path, data, -1, NULL);
  g_chmod (path, 0600);
#endif
}

static gboolean
load_tokens (SpotifyAuth *self)
{
  g_autofree gchar *path = g_build_filename (g_get_user_config_dir (),
                                             "spotifygtk", "tokens", NULL);
  g_autofree gchar *data = NULL;
  if (!g_file_get_contents (path, &data, NULL, NULL))
    return FALSE;

  gchar **parts = g_strsplit (g_strstrip (data), "\n", 3);
  if (g_strv_length (parts) < 3) { g_strfreev (parts); return FALSE; }

  g_free (self->access_token);
  g_free (self->refresh_token);
  self->access_token  = g_strdup (parts[0]);
  self->refresh_token = g_strdup (parts[1]);
  self->expires_at    = g_ascii_strtoll (parts[2], NULL, 10);
  g_strfreev (parts);
  return TRUE;
}

static void
on_token_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyAuth       *self = SPOTIFYGTK_AUTH (user_data);
  g_autoptr(GError)  err  = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  if (!bytes) {
    g_warning ("Token request failed: %s", err ? err->message : "unknown");
    g_signal_emit (self, signals[SIG_COMPLETED], 0, FALSE);
    return;
  }

  gsize   len  = 0;
  const gchar *body = g_bytes_get_data (bytes, &len);

  g_autoptr(JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body, (gssize) len, &err)) {
    g_warning ("JSON parse error: %s", err->message);
    g_signal_emit (self, signals[SIG_COMPLETED], 0, FALSE);
    g_bytes_unref (bytes);
    return;
  }

  JsonObject *root = json_node_get_object (json_parser_get_root (parser));

  if (json_object_has_member (root, "error")) {
    g_warning ("Spotify token error: %s",
               json_object_get_string_member (root, "error_description"));
    g_signal_emit (self, signals[SIG_COMPLETED], 0, FALSE);
    g_bytes_unref (bytes);
    return;
  }

  g_free (self->access_token);
  g_free (self->refresh_token);
  self->access_token  = g_strdup (json_object_get_string_member (root, "access_token"));
  self->refresh_token = g_strdup (json_object_get_string_member (root, "refresh_token"));
  gint64 expires_in   = json_object_get_int_member (root, "expires_in");
  self->expires_at    = g_get_real_time () / G_USEC_PER_SEC + expires_in - 60;

  store_tokens (self);
  g_bytes_unref (bytes);

  g_signal_emit (self, signals[SIG_COMPLETED], 0, TRUE);
}

static void
exchange_code (SpotifyAuth *self, const gchar *code)
{
  const gchar *client_id = g_getenv ("SPOTIFY_CLIENT_ID");
  if (!client_id || *client_id == '\0') {
    g_warning ("SPOTIFY_CLIENT_ID env var not set");
    g_signal_emit (self, signals[SIG_COMPLETED], 0, FALSE);
    return;
  }

  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, SPOTIFY_TOKEN_URL);

  g_autofree gchar *body =
    g_strdup_printf ("grant_type=authorization_code&code=%s&redirect_uri=%s"
                     "&client_id=%s&code_verifier=%s",
                     code, REDIRECT_URI, client_id, self->code_verifier);

  GBytes *bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", bytes);
  g_bytes_unref (bytes);

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, NULL,
                                    on_token_response, self);
  g_object_unref (msg);
}

static void
on_callback_request (SoupServer *server, SoupServerMessage *msg, const gchar *path,
                     GHashTable *query, gpointer user_data)
{
  SpotifyAuth *self = SPOTIFYGTK_AUTH (user_data);
  if (g_strcmp0 (path, "/callback") != 0) return;

  const gchar *code  = query ? g_hash_table_lookup (query, "code")  : NULL;
  const gchar *state = query ? g_hash_table_lookup (query, "state") : NULL;
  const gchar *error = query ? g_hash_table_lookup (query, "error") : NULL;

  const gchar *html_ok =
    "<html><body style='font-family:sans-serif;text-align:center;padding:4em'>"
    "<h2>Authenticated</h2><p>You can close this tab.</p></body></html>";
  const gchar *html_err =
    "<html><body style='font-family:sans-serif;text-align:center;padding:4em'>"
    "<h2>Authentication failed</h2><p>Check SpotifyGTK for details.</p></body></html>";

  if (error || !code) {
    g_warning ("OAuth callback error: %s", error ? error : "no code");
    soup_server_message_set_status (msg, 400, NULL);
    soup_server_message_set_response (msg, "text/html", SOUP_MEMORY_STATIC, html_err, strlen (html_err));
    g_signal_emit (self, signals[SIG_COMPLETED], 0, FALSE);
    goto cleanup;
  }

  if (g_strcmp0 (state, self->state_nonce) != 0) {
    g_warning ("OAuth state mismatch - possible CSRF attempt");
    soup_server_message_set_status (msg, 400, NULL);
    soup_server_message_set_response (msg, "text/html", SOUP_MEMORY_STATIC, html_err, strlen (html_err));
    g_signal_emit (self, signals[SIG_COMPLETED], 0, FALSE);
    goto cleanup;
  }

  soup_server_message_set_status (msg, 200, NULL);
  soup_server_message_set_response (msg, "text/html", SOUP_MEMORY_STATIC, html_ok, strlen (html_ok));
  exchange_code (self, code);

cleanup:
  soup_server_remove_handler (server, "/callback");
  soup_server_disconnect (server);
}

gboolean
spotifygtk_auth_has_valid_token (SpotifyAuth *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_AUTH (self), FALSE);
  if (!self->access_token && !load_tokens (self))
    return FALSE;
  return (g_get_real_time () / G_USEC_PER_SEC) < self->expires_at;
}

const gchar *
spotifygtk_auth_get_token (SpotifyAuth *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_AUTH (self), NULL);
  return self->access_token;
}

void
spotifygtk_auth_begin (SpotifyAuth *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_AUTH (self));

  const gchar *client_id = g_getenv ("SPOTIFY_CLIENT_ID");
  if (!client_id || *client_id == '\0') {
    g_warning ("Set SPOTIFY_CLIENT_ID before launching SpotifyGTK");
    g_signal_emit (self, signals[SIG_COMPLETED], 0, FALSE);
    return;
  }

  g_free (self->code_verifier);
  g_free (self->state_nonce);
  self->code_verifier = generate_random_base64url (64);
  self->state_nonce   = generate_random_base64url (16);

  g_autofree gchar *code_challenge = sha256_base64url (self->code_verifier);

  g_autoptr(GError) err = NULL;
  self->redirect_server = soup_server_new (NULL, NULL);
  soup_server_add_handler (self->redirect_server, "/callback", on_callback_request, self, NULL);

  if (!soup_server_listen_local (self->redirect_server, REDIRECT_PORT,
                                 SOUP_SERVER_LISTEN_IPV4_ONLY, &err)) {
    g_warning ("Could not bind OAuth listener: %s", err->message);
    g_signal_emit (self, signals[SIG_COMPLETED], 0, FALSE);
    return;
  }

  g_autofree gchar *url =
    g_strdup_printf ("%s?response_type=code&client_id=%s&scope=%s&redirect_uri=%s"
                     "&state=%s&code_challenge_method=S256&code_challenge=%s",
                     SPOTIFY_AUTH_URL, client_id, SPOTIFY_SCOPES, REDIRECT_URI,
                     self->state_nonce, code_challenge);

  g_message ("Opening browser for Spotify authorization...");
  g_app_info_launch_default_for_uri (url, NULL, NULL);
}

void
spotifygtk_auth_refresh (SpotifyAuth *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_AUTH (self));
  if (!self->refresh_token) { spotifygtk_auth_begin (self); return; }

  const gchar *client_id = g_getenv ("SPOTIFY_CLIENT_ID");
  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, SPOTIFY_TOKEN_URL);

  g_autofree gchar *body =
    g_strdup_printf ("grant_type=refresh_token&refresh_token=%s&client_id=%s",
                     self->refresh_token, client_id);

  GBytes *bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", bytes);
  g_bytes_unref (bytes);

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, NULL,
                                    on_token_response, self);
  g_object_unref (msg);
}

void
spotifygtk_auth_revoke (SpotifyAuth *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_AUTH (self));
  g_clear_pointer (&self->access_token,  g_free);
  g_clear_pointer (&self->refresh_token, g_free);
  self->expires_at = 0;

  g_autofree gchar *path = g_build_filename (g_get_user_config_dir (), "spotifygtk", "tokens", NULL);
  g_remove (path);
}

static void
spotifygtk_auth_dispose (GObject *object)
{
  SpotifyAuth *self = SPOTIFYGTK_AUTH (object);
  g_clear_object (&self->session);
  g_clear_object (&self->redirect_server);
  G_OBJECT_CLASS (spotifygtk_auth_parent_class)->dispose (object);
}

static void
spotifygtk_auth_finalize (GObject *object)
{
  SpotifyAuth *self = SPOTIFYGTK_AUTH (object);
  g_free (self->code_verifier);
  g_free (self->state_nonce);
  g_free (self->access_token);
  g_free (self->refresh_token);
  G_OBJECT_CLASS (spotifygtk_auth_parent_class)->finalize (object);
}

static void
spotifygtk_auth_class_init (SpotifyAuthClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose  = spotifygtk_auth_dispose;
  object_class->finalize = spotifygtk_auth_finalize;

  signals[SIG_COMPLETED] =
    g_signal_new ("completed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
spotifygtk_auth_init (SpotifyAuth *self)
{
  self->session = soup_session_new_with_options ("user-agent", "SpotifyGTK/" APP_VERSION, NULL);
}

SpotifyAuth *
spotifygtk_auth_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_AUTH, NULL);
}

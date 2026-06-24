#include "config.h"
#include "api.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

struct _SpotifyApi {
  GObject      parent_instance;
  SpotifyAuth *auth;
  SoupSession *session;
};

G_DEFINE_FINAL_TYPE (SpotifyApi, spotifygtk_api, G_TYPE_OBJECT)

typedef struct {
  SpotifyApi         *api;
  SpotifyApiCallback  callback;
  gpointer            user_data;
  SoupMessage        *msg;     /* kept alive so we can read status after the read completes */
  gchar              *method;
  gchar              *url;
} RequestClosure;

static void
on_api_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RequestClosure    *cl   = user_data;
  g_autoptr(GError)  err  = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);

  guint        status = soup_message_get_status (cl->msg);
  const gchar *reason = soup_message_get_reason_phrase (cl->msg);

  if (!bytes) {
    g_warning ("[api] %s %s -> transport error: %s",
              cl->method, cl->url, err ? err->message : "unknown");
    if (cl->callback) cl->callback (cl->api, NULL, err, cl->user_data);
    goto cleanup;
  }

  gsize        len  = 0;
  const gchar *body = g_bytes_get_data (bytes, &len);

  g_message ("[api] %s %s -> HTTP %u %s (%" G_GSIZE_FORMAT " bytes)",
            cl->method, cl->url, status, reason ? reason : "", len);

  JsonObject *root = NULL;
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GError) parse_err = NULL;

  if (len > 0) {
    if (json_parser_load_from_data (parser, body, (gssize) len, &parse_err)) {
      JsonNode *node = json_parser_get_root (parser);
      if (node && JSON_NODE_HOLDS_OBJECT (node))
        root = json_node_get_object (node);
    } else {
      /* Spotify doesn't always return JSON errors -- account-level
       * restrictions (like a Premium gate) come back as plain text.
       * Surface that clearly instead of failing to parse it silently. */
      g_autofree gchar *snippet = g_strndup (body, MIN (len, 300));
      g_message ("[api] non-JSON response body (HTTP %u): %s", status, snippet);
    }
  }

  if (status >= 400) {
    g_autofree gchar *detail = NULL;
    if (root && json_object_has_member (root, "error")) {
      JsonNode *enode = json_object_get_member (root, "error");
      if (JSON_NODE_HOLDS_OBJECT (enode))
        detail = g_strdup (json_object_get_string_member_with_default (
                  json_node_get_object (enode), "message", NULL));
    }
    if (!detail) detail = g_strndup (body, MIN (len, 300));

    GError *http_err = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
      "HTTP %u %s: %s", status, reason ? reason : "", detail ? detail : "(no body)");
    g_warning ("[api] request failed: %s", http_err->message);
    if (cl->callback) cl->callback (cl->api, root, http_err, cl->user_data);
    g_error_free (http_err);
    g_bytes_unref (bytes);
    goto cleanup;
  }

  if (cl->callback) cl->callback (cl->api, root, NULL, cl->user_data);
  g_bytes_unref (bytes);

cleanup:
  g_object_unref (cl->msg);
  g_free (cl->method);
  g_free (cl->url);
  g_free (cl);
}

static void
api_request (SpotifyApi *self, const gchar *method, const gchar *endpoint,
            const gchar *body_json, SpotifyApiCallback callback, gpointer user_data)
{
  g_autofree gchar *url = g_strdup_printf ("%s%s", SPOTIFY_API_BASE, endpoint);
  SoupMessage *msg = soup_message_new (method, url);

  const gchar *token = spotifygtk_auth_get_token (self->auth);
  if (!token) {
    g_warning ("[api] %s %s attempted with no access token -- auth hasn't completed yet", method, url);
  }
  g_autofree gchar *auth_hdr = g_strdup_printf ("Bearer %s", token ? token : "");
  soup_message_headers_replace (soup_message_get_request_headers (msg), "Authorization", auth_hdr);

  if (body_json) {
    GBytes *bytes = g_bytes_new (body_json, strlen (body_json));
    soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
    g_bytes_unref (bytes);
  }

  g_message ("[api] -> %s %s%s%s", method, url, body_json ? " body=" : "", body_json ? body_json : "");

  RequestClosure *cl = g_new0 (RequestClosure, 1);
  cl->api       = self;
  cl->callback  = callback;
  cl->user_data = user_data;
  cl->msg       = g_object_ref (msg);
  cl->method    = g_strdup (method);
  cl->url       = g_strdup (url);

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, NULL,
                                    on_api_response, cl);
  g_object_unref (msg);
}

void spotifygtk_api_get_playback_state (SpotifyApi *self, SpotifyApiCallback cb, gpointer data)
{ api_request (self, SOUP_METHOD_GET, "/me/player", NULL, cb, data); }

void spotifygtk_api_play (SpotifyApi *self, const gchar *context_uri, SpotifyApiCallback cb, gpointer data)
{
  g_autofree gchar *body = context_uri ? g_strdup_printf ("{\"context_uri\":\"%s\"}", context_uri) : NULL;
  api_request (self, SOUP_METHOD_PUT, "/me/player/play", body, cb, data);
}
void spotifygtk_api_play_track (SpotifyApi *self, const gchar *track_uri, SpotifyApiCallback cb, gpointer data)
{
  g_autofree gchar *body = g_strdup_printf ("{\"uris\":[\"%s\"]}", track_uri);
  api_request (self, SOUP_METHOD_PUT, "/me/player/play", body, cb, data);
}

void spotifygtk_api_pause (SpotifyApi *self, SpotifyApiCallback cb, gpointer data)
{ api_request (self, SOUP_METHOD_PUT, "/me/player/pause", NULL, cb, data); }

void spotifygtk_api_next (SpotifyApi *self, SpotifyApiCallback cb, gpointer data)
{ api_request (self, SOUP_METHOD_POST, "/me/player/next", NULL, cb, data); }

void spotifygtk_api_previous (SpotifyApi *self, SpotifyApiCallback cb, gpointer data)
{ api_request (self, SOUP_METHOD_POST, "/me/player/previous", NULL, cb, data); }

void spotifygtk_api_set_volume (SpotifyApi *self, gint percent, SpotifyApiCallback cb, gpointer data)
{
  g_autofree gchar *ep = g_strdup_printf ("/me/player/volume?volume_percent=%d", percent);
  api_request (self, SOUP_METHOD_PUT, ep, NULL, cb, data);
}

void spotifygtk_api_seek (SpotifyApi *self, gint64 position_ms, SpotifyApiCallback cb, gpointer data)
{
  g_autofree gchar *ep = g_strdup_printf ("/me/player/seek?position_ms=%" G_GINT64_FORMAT, position_ms);
  api_request (self, SOUP_METHOD_PUT, ep, NULL, cb, data);
}

void spotifygtk_api_get_user_playlists (SpotifyApi *self, gint limit, gint offset, SpotifyApiCallback cb, gpointer data)
{
  g_autofree gchar *ep = g_strdup_printf ("/me/playlists?limit=%d&offset=%d", limit, offset);
  api_request (self, SOUP_METHOD_GET, ep, NULL, cb, data);
}

void spotifygtk_api_get_playlist_tracks (SpotifyApi *self, const gchar *playlist_id, SpotifyApiCallback cb, gpointer data)
{
  g_autofree gchar *ep = g_strdup_printf ("/playlists/%s/tracks", playlist_id);
  api_request (self, SOUP_METHOD_GET, ep, NULL, cb, data);
}

void spotifygtk_api_get_saved_tracks (SpotifyApi *self, gint limit, gint offset, SpotifyApiCallback cb, gpointer data)
{
  g_autofree gchar *ep = g_strdup_printf ("/me/tracks?limit=%d&offset=%d", limit, offset);
  api_request (self, SOUP_METHOD_GET, ep, NULL, cb, data);
}

void spotifygtk_api_search (SpotifyApi *self, const gchar *query, const gchar *types, SpotifyApiCallback cb, gpointer data)
{
  g_autofree gchar *encoded = g_uri_escape_string (query, NULL, FALSE);
  g_autofree gchar *ep = g_strdup_printf ("/search?q=%s&type=%s&limit=20", encoded, types);
  api_request (self, SOUP_METHOD_GET, ep, NULL, cb, data);
}

void spotifygtk_api_get_current_user (SpotifyApi *self, SpotifyApiCallback cb, gpointer data)
{ api_request (self, SOUP_METHOD_GET, "/me", NULL, cb, data); }

static void
spotifygtk_api_dispose (GObject *object)
{
  SpotifyApi *self = SPOTIFYGTK_API (object);
  g_clear_object (&self->auth);
  g_clear_object (&self->session);
  G_OBJECT_CLASS (spotifygtk_api_parent_class)->dispose (object);
}

static void
spotifygtk_api_class_init (SpotifyApiClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_api_dispose;
}

static void
spotifygtk_api_init (SpotifyApi *self)
{
  self->session = soup_session_new_with_options ("user-agent", "SpotifyGTK/" APP_VERSION, NULL);
}

SpotifyApi *
spotifygtk_api_new (SpotifyAuth *auth)
{
  SpotifyApi *self = g_object_new (SPOTIFYGTK_TYPE_API, NULL);
  self->auth = g_object_ref (auth);
  return self;
}
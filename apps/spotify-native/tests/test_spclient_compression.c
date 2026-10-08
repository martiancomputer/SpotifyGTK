/* Synthetic, unauthenticated loopback regression tests. No captured traffic,
 * credentials, catalogue identifiers, or external service is used here.
 * Include the implementation to test both private session creation paths and
 * the real metadata/context callbacks, redirecting only message destinations.
 */
#include <libsoup/soup.h>

static gchar *server_url;
static SoupMessage *loopback_message_new (const gchar *method, const gchar *url);
#define soup_message_new loopback_message_new
#include "../src/spotify/spclient.c"
#undef soup_message_new

#define TRACK_COUNT 2000
#define JSON_PADDING (2 * 1024 * 1024)

typedef struct {
  SoupServer *server;
  GBytes *body;
  GBytes *gzip;
  GBytes *zstd;
  const gchar *content_type;
  guint status;
  guint requests;
  gboolean identity;
  gboolean redirect;
  GMainLoop *loop;
  gboolean done;
  GError *error;
  guint tracks;
  JsonNode *context;
} Fixture;

static SoupMessage *
loopback_message_new (const gchar *method, const gchar *url)
{
  g_assert_nonnull (server_url);
  /* No request from this test can leave the loopback server. */
  return soup_message_new (method, server_url);
}

static GBytes *
compress_gzip (GBytes *plain)
{
  g_autoptr(GOutputStream) memory = g_memory_output_stream_new_resizable ();
  g_autoptr(GZlibCompressor) compressor =
    g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, 6);
  g_autoptr(GOutputStream) stream =
    g_converter_output_stream_new (memory, G_CONVERTER (compressor));
  gsize len;
  const guint8 *data = g_bytes_get_data (plain, &len);
  g_autoptr(GError) error = NULL;
  g_assert_true (g_output_stream_write_all (stream, data, len, NULL, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_output_stream_close (stream, NULL, &error));
  g_assert_no_error (error);
  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (memory));
}

static GBytes *
concatenated_zstd (GBytes *plain)
{
  /* Valid single-segment Zstandard frames with one final raw block each.
   * Use the observed 64 KiB boundaries without adding a libzstd dependency
   * or committing any real response. A client that advertises zstd instead
   * of the gzip-only policy gets this representation from the test server.
   */
  g_autoptr(GByteArray) frames = g_byte_array_new ();
  gsize len;
  const guint8 *data = g_bytes_get_data (plain, &len);
  for (gsize pos = 0; pos < len; pos += 65536) {
    guint32 size = MIN (len - pos, 65536);
    guint32 block = (size << 3) | 1; /* raw, last block */
    guint8 header[] = {
      0x28, 0xb5, 0x2f, 0xfd, 0xa0, /* magic, 4-byte content size */
      size & 255, (size >> 8) & 255, (size >> 16) & 255, (size >> 24) & 255,
      block & 255, (block >> 8) & 255, (block >> 16) & 255,
    };
    g_byte_array_append (frames, header, sizeof header);
    g_byte_array_append (frames, data + pos, size);
  }
  return g_byte_array_free_to_bytes (g_steal_pointer (&frames));
}

static GBytes *
metadata_body (void)
{
  g_autoptr(GByteArray) array = g_byte_array_new ();
  pb_write_varint_field (array, 2, 10); /* TRACK_V4 */
  const guint8 padding[384] = { 0 };
  for (guint i = 0; i < TRACK_COUNT; i++) {
    g_autofree gchar *uri = g_strdup_printf ("spotify:track:fixture%u", i);
    g_autofree gchar *name = g_strdup_printf ("Fixture track %u", i);
    g_autoptr(GByteArray) track = g_byte_array_new ();
    pb_write_bytes_field (track, 2, (const guint8 *) name, strlen (name));
    pb_write_varint_field (track, 7, 180000 * 2); /* sint32 duration */
    pb_write_bytes_field (track, 99, padding, sizeof padding);
    g_autoptr(GByteArray) any = g_byte_array_new ();
    pb_write_message_field (any, 2, track->data, track->len);
    g_autoptr(GByteArray) entity = g_byte_array_new ();
    pb_write_bytes_field (entity, 2, (const guint8 *) uri, strlen (uri));
    pb_write_message_field (entity, 3, any->data, any->len);
    pb_write_message_field (array, 3, entity->data, entity->len);
  }
  g_autoptr(GByteArray) batch = g_byte_array_new ();
  pb_write_message_field (batch, 2, array->data, array->len);
  return g_byte_array_free_to_bytes (g_steal_pointer (&batch));
}

static GBytes *
json_body (void)
{
  g_autoptr(GString) json = g_string_new ("{\"padding\":\"");
  for (guint i = 0; i < JSON_PADDING; i++)
    g_string_append_c (json, 'a' + i % 26);
  g_string_append (json, "\",\"tail\":\"complete\",\"pages\":[{\"tracks\":[]}]}");
  return g_bytes_new (json->str, json->len);
}

static void
serve_response (SoupServer *server, SoupServerMessage *message,
                const gchar *path, GHashTable *query, gpointer user_data)
{
  Fixture *f = user_data;
  f->requests++;
  const gchar *accept = soup_message_headers_get_one (
    soup_server_message_get_request_headers (message), "Accept-Encoding");
  gboolean gzip = g_strcmp0 (accept, "gzip") == 0;
  g_assert_cmpstr (accept, ==, "gzip");
  if (f->redirect && f->requests == 1) {
    g_autofree gchar *location = g_strconcat (server_url, "redirected", NULL);
    soup_server_message_set_redirect (message, SOUP_STATUS_FOUND, location);
    return;
  }
  GBytes *representation = f->identity ? f->body : gzip ? f->gzip : f->zstd;
  gsize len;
  const gchar *data = g_bytes_get_data (representation, &len);
  soup_server_message_set_status (message, f->status, NULL);
  soup_server_message_set_response (message, f->content_type,
                                    SOUP_MEMORY_COPY, data, len);
  if (!f->identity)
    soup_message_headers_replace (soup_server_message_get_response_headers (message),
                                  "Content-Encoding", gzip ? "gzip" : "zstd");
}

static void
fixture_init (Fixture *f, GBytes *body, const gchar *type, guint status)
{
  f->body = body;
  f->gzip = compress_gzip (body);
  f->zstd = concatenated_zstd (body);
  f->content_type = type;
  f->status = status;
  f->loop = g_main_loop_new (NULL, FALSE);
  f->server = soup_server_new (NULL, NULL);
  soup_server_add_handler (f->server, NULL, serve_response, f, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true (soup_server_listen_local (f->server, 0,
    SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  g_assert_no_error (error);
  GSList *uris = soup_server_get_uris (f->server);
  server_url = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
}

static gboolean
timeout (gpointer data)
{
  g_error ("loopback catalogue request did not complete");
  return G_SOURCE_REMOVE;
}

static void
fixture_wait (Fixture *f)
{
  guint timer = g_timeout_add_seconds (5, timeout, NULL);
  if (!f->done)
    g_main_loop_run (f->loop);
  g_source_remove (timer);
  g_assert_true (f->done);
}

static void
fixture_clear (Fixture *f)
{
  soup_server_disconnect (f->server);
  g_object_unref (f->server);
  g_bytes_unref (f->body);
  g_bytes_unref (f->gzip);
  g_bytes_unref (f->zstd);
  g_clear_pointer (&f->context, json_node_free);
  g_clear_error (&f->error);
  g_main_loop_unref (f->loop);
  g_clear_pointer (&server_url, g_free);
}

static void
metadata_done (const SpclientTrackInfo *tracks, guint n, GError *error, gpointer data)
{
  Fixture *f = data;
  f->tracks = n;
  if (error)
    f->error = g_error_copy (error);
  for (guint i = 0; i < n; i++) {
    g_autofree gchar *name = g_strdup_printf ("Fixture track %u", i);
    g_assert_cmpstr (tracks[i].meta.name, ==, name);
    g_assert_cmpint (tracks[i].meta.duration_ms, ==, 180000);
  }
  f->done = TRUE;
  g_main_loop_quit (f->loop);
}

static void
context_done (JsonNode *context, GError *error, gpointer data)
{
  Fixture *f = data;
  f->context = context;
  if (error)
    f->error = g_error_copy (error);
  f->done = TRUE;
  g_main_loop_quit (f->loop);
}

static void
test_metadata (gconstpointer data)
{
  guint status = GPOINTER_TO_UINT (data);
  Fixture f = { 0 };
  fixture_init (&f, metadata_body (), "application/x-protobuf", status);
  g_assert_cmpuint (g_bytes_get_size (f.body), >, 65536);
  g_autoptr(SpotifySpclient) client = spotifygtk_spclient_new ();
  g_auto(GStrv) uris = g_new0 (gchar *, TRACK_COUNT + 1);
  for (guint i = 0; i < TRACK_COUNT; i++)
    uris[i] = g_strdup_printf ("spotify:track:fixture%u", i);
  spotifygtk_spclient_get_tracks_metadata (client, (const gchar * const *) uris,
    TRACK_COUNT, NULL, NULL, metadata_done, &f);
  fixture_wait (&f);
  g_assert_cmpuint (f.requests, ==, 1);
  if (status == SOUP_STATUS_OK) {
    g_assert_no_error (f.error);
    g_assert_cmpuint (f.tracks, ==, TRACK_COUNT);
  } else {
    g_assert_error (f.error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull (strstr (f.error->message, "HTTP 429"));
    g_assert_cmpuint (f.tracks, ==, 0);
  }
  fixture_clear (&f);
}

static void
test_context (gconstpointer data)
{
  guint status = GPOINTER_TO_UINT (data);
  Fixture f = { 0 };
  fixture_init (&f, json_body (), "application/json", status);
  g_autoptr(SpotifySpclient) client = spotifygtk_spclient_new ();
  request_context_resolve (client, "spotify:playlist:fixture", NULL, NULL, context_done, &f);
  fixture_wait (&f);
  if (status == SOUP_STATUS_OK) {
    g_assert_no_error (f.error);
    g_assert_nonnull (f.context);
    JsonObject *object = json_node_get_object (f.context);
    g_assert_cmpstr (json_object_get_string_member (object, "tail"), ==, "complete");
    g_assert_cmpuint (strlen (json_object_get_string_member (object, "padding")), ==, JSON_PADDING);
  } else {
    g_assert_error (f.error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull (strstr (f.error->message, "HTTP 503"));
    g_assert_null (f.context);
  }
  fixture_clear (&f);
}

static void
session_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Fixture *f = data;
  g_autoptr(GBytes) bytes = soup_session_send_and_read_finish (
    SOUP_SESSION (source), result, &f->error);
  g_assert_no_error (f->error);
  g_assert_nonnull (bytes);
  g_assert_true (g_bytes_equal (bytes, f->body));
  f->done = TRUE;
  g_main_loop_quit (f->loop);
}

static void
test_pathfinder (gconstpointer data)
{
  Fixture f = { 0 };
  fixture_init (&f, json_body (), "application/json", SOUP_STATUS_OK);
  f.identity = GPOINTER_TO_UINT (data) == 1;
  f.redirect = GPOINTER_TO_UINT (data) == 2;
  g_autoptr(SoupSession) session = new_pathfinder_session ();
  g_autoptr(SoupMessage) message = soup_message_new (SOUP_METHOD_GET, server_url);
  /* Even an existing broad header must not re-enable the broken decoder. */
  soup_message_headers_replace (soup_message_get_request_headers (message),
                                "Accept-Encoding", "gzip, br, zstd");
  soup_session_send_and_read_async (session, message, G_PRIORITY_DEFAULT,
                                    NULL, session_done, &f);
  fixture_wait (&f);
  g_assert_cmpuint (f.requests, ==, f.redirect ? 2 : 1);
  fixture_clear (&f);
}

static void
test_cancelled (gconstpointer data)
{
  Fixture f = { 0 };
  fixture_init (&f, metadata_body (), "application/x-protobuf", SOUP_STATUS_OK);
  g_autoptr(SpotifySpclient) client = spotifygtk_spclient_new ();
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_cancellable_cancel (cancellable);
  spotifygtk_spclient_set_cancellable (client, cancellable);
  if (GPOINTER_TO_UINT (data)) {
    request_context_resolve (client, "spotify:playlist:fixture", NULL, NULL, context_done, &f);
  } else {
    const gchar *uris[] = { "spotify:track:fixture" };
    spotifygtk_spclient_get_tracks_metadata (client, uris, 1, NULL, NULL, metadata_done, &f);
  }
  fixture_wait (&f);
  g_assert_error (f.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (f.tracks, ==, 0);
  g_assert_cmpuint (f.requests, ==, 0);
  fixture_clear (&f);
}

static void
test_invalid_json_is_private (void)
{
  Fixture f = { 0 };
  fixture_init (&f, g_bytes_new_static ("private-response-marker", 23),
                "application/json", SOUP_STATUS_OK);
  g_autoptr(SpotifySpclient) client = spotifygtk_spclient_new ();
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
    "*context-resolve HTTP 200 response was not valid JSON (23 decoded bytes)*");
  request_context_resolve (client, "spotify:playlist:fixture", NULL, NULL, context_done, &f);
  fixture_wait (&f);
  g_test_assert_expected_messages ();
  g_assert_error (f.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (strstr (f.error->message, "private-response-marker"));
  fixture_clear (&f);
}

static void
test_home_transport (gconstpointer data)
{
  guint mode = GPOINTER_TO_UINT (data);
  Fixture f = {0};
  GBytes *body;
  if (mode == 1) {
    g_autoptr(GString) large = g_string_sized_new (5 * 1024 * 1024);
    for (guint i = 0; i < 5 * 1024 * 1024; i++) g_string_append_c (large, 'x');
    body = g_bytes_new (large->str, large->len);
  } else if (mode == 2)
    body = g_bytes_new_static ("private-response-marker", 23);
  else if (mode == 5) {
    g_autoptr(GString) deep = g_string_new (NULL);
    for (guint i = 0; i < 70; i++) g_string_append_c (deep, '[');
    g_string_append_c (deep, '0');
    for (guint i = 0; i < 70; i++) g_string_append_c (deep, ']');
    body = g_bytes_new (deep->str, deep->len);
  } else body = json_body ();
  fixture_init (&f, body, "application/json", mode == 4 ? 503 : 200);
  f.redirect = mode == 3;
  g_autoptr(SpotifySpclient) client = spotifygtk_spclient_new ();
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  if (mode == 6) g_cancellable_cancel (cancel);
  spotifygtk_spclient_get_home (client, "UTC", NULL, NULL, cancel, context_done, &f);
  fixture_wait (&f);
  g_assert_cmpuint (f.requests, ==, mode == 6 ? 0 : 1);
  if (!mode) {
    g_assert_no_error (f.error);
    g_assert_nonnull (f.context);
    g_assert_cmpstr (json_object_get_string_member (json_node_get_object (f.context), "tail"), ==, "complete");
  } else {
    g_assert_nonnull (f.error);
    g_assert_null (f.context);
    g_assert_null (strstr (f.error->message, "private-response-marker"));
    if (mode == 6) g_assert_error (f.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  }
  fixture_clear (&f);
}

static void
test_home_body (void)
{
  g_autofree gchar *body = spotifygtk_spclient_build_home_body ("UTC");
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, body, -1, NULL));
  JsonObject *root = json_node_get_object (json_parser_get_root (parser));
  g_assert_cmpstr (json_object_get_string_member (root, "operationName"), ==, "home");
  JsonObject *variables = json_object_get_object_member (root, "variables");
  g_assert_cmpstr (json_object_get_string_member (variables, "sp_t"), ==, "");
  g_assert_cmpint (json_object_get_int_member (variables, "sectionItemsLimit"), ==, 10);
  g_assert_cmpstr (json_object_get_string_member (variables, "homeEndUserIntegration"), ==, "INTEGRATION_DESKTOP");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/spclient/compression/large-metadata", GUINT_TO_POINTER (200), test_metadata);
  g_test_add_data_func ("/spclient/compression/metadata-http-error", GUINT_TO_POINTER (429), test_metadata);
  g_test_add_data_func ("/spclient/compression/large-context", GUINT_TO_POINTER (200), test_context);
  g_test_add_data_func ("/spclient/compression/context-http-error", GUINT_TO_POINTER (503), test_context);
  g_test_add_data_func ("/spclient/compression/large-pathfinder", GUINT_TO_POINTER (0), test_pathfinder);
  g_test_add_data_func ("/spclient/compression/pathfinder-identity", GUINT_TO_POINTER (1), test_pathfinder);
  g_test_add_data_func ("/spclient/compression/pathfinder-redirect", GUINT_TO_POINTER (2), test_pathfinder);
  g_test_add_data_func ("/spclient/compression/metadata-cancellation", GUINT_TO_POINTER (0), test_cancelled);
  g_test_add_data_func ("/spclient/compression/context-cancellation", GUINT_TO_POINTER (1), test_cancelled);
  g_test_add_func ("/spclient/compression/private-json-error", test_invalid_json_is_private);
  g_test_add_func ("/spclient/home/request-body", test_home_body);
  const gchar *modes[] = { "gzip", "size-limit", "invalid-json", "no-redirect", "http-error", "depth-limit", "cancelled" };
  for (guint i = 0; i < G_N_ELEMENTS (modes); i++) {
    g_autofree gchar *path = g_strconcat ("/spclient/home/", modes[i], NULL);
    g_test_add_data_func (path, GUINT_TO_POINTER (i), test_home_transport);
  }
  return g_test_run ();
}

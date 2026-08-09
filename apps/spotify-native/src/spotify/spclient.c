/*
 * spclient.c — HTTPS client for track metadata / CDN URL resolution.
 * See spclient.h for scope and the apresolve simplification note.
 */

#include "config.h"
#include "spclient.h"

#include <stdlib.h>
#include "protobuf_min.h"

#include <libsoup/soup.h>
#include <string.h>

struct _SpotifySpclient {
  GObject      parent_instance;
  SoupSession *session;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (SpotifySpclient, spotifygtk_spclient, G_TYPE_OBJECT)

typedef struct {
  SpclientStorageCallback callback;
  gpointer                user_data;
} RequestClosure;

typedef struct {
  SpclientTrackCallback callback;
  gpointer              user_data;
} TrackRequestClosure;

static gchar *
bytes_to_hex (const guint8 *data, gsize len)
{
  GString *s = g_string_sized_new (len * 2);
  for (gsize i = 0; i < len; i++)
    g_string_append_printf (s, "%02x", data[i]);
  return g_string_free (s, FALSE);
}

void
spclient_cdn_urls_free (SpclientCdnUrls *urls)
{
  if (!urls) return;
  g_strfreev (urls->urls);
  g_free (urls);
}

static void
on_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RequestClosure    *cl  = user_data;
  g_autoptr(GError)  err = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  if (!bytes) {
    if (cl->callback) cl->callback (NULL, err, cl->user_data);
    g_free (cl);
    return;
  }

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);

  /* StorageResolveResponse: result (field 1, varint enum -- CDN=0,
   * STORAGE=1, RESTRICTED=3), cdnurl (field 2, REPEATED string),
   * fileid (field 4, bytes). Repeated fields need walking all
   * top-level entries via pb_read_field directly -- protobuf_min's
   * pb_find_*_field helpers only return the first match, which is
   * correct for every other message in this codebase so far (none
   * of them use repeated fields) but not here. */
  GPtrArray *url_list = g_ptr_array_new ();
  gsize pos = 0;
  guint32 field_num;
  PbWireType wire_type;
  const guint8 *field_data;
  gsize field_len;
  guint64 field_varint;

  while (pb_read_field (data, len, &pos, &field_num, &wire_type,
                        &field_data, &field_len, &field_varint)) {
    if (field_num == 2 && wire_type == PB_WIRE_LENGTH_DELIMITED)
      g_ptr_array_add (url_list, g_strndup ((const gchar *) field_data, field_len));
  }

  if (url_list->len == 0) {
    g_ptr_array_free (url_list, TRUE);
    GError *no_urls = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
      "storage-resolve response contained no cdnurl entries "
      "(RESTRICTED result, or an unparseable/error response)");
    g_warning ("spclient: %s", no_urls->message);
    if (cl->callback) cl->callback (NULL, no_urls, cl->user_data);
    g_error_free (no_urls);
    g_bytes_unref (bytes);
    g_free (cl);
    return;
  }

  g_ptr_array_add (url_list, NULL);  /* NULL-terminate for g_strfreev */

  SpclientCdnUrls *urls = g_new0 (SpclientCdnUrls, 1);
  urls->n_urls = url_list->len - 1;
  urls->urls = (gchar **) g_ptr_array_free (url_list, FALSE);  /* transfer ownership */

  g_message ("spclient: resolved %u CDN URL(s)", urls->n_urls);
  if (cl->callback) cl->callback (urls, NULL, cl->user_data);

  g_bytes_unref (bytes);
  g_free (cl);
}

static void
extract_audio_files_from_track (const guint8 *track_data, gsize track_len,
                                const guint8 *parent_gid, gsize parent_gid_len,
                                GArray *files)
{
  gsize pos = 0;
  guint32 field_num;
  PbWireType wire_type;
  const guint8 *field_data;
  gsize field_len;
  guint64 field_varint;

  const guint8 *current_gid = parent_gid;
  gsize current_gid_len = parent_gid_len;

  /* First pass: find gid (field 1) of the current track, if any */
  gsize temp_pos = 0;
  while (pb_read_field (track_data, track_len, &temp_pos, &field_num, &wire_type,
                        &field_data, &field_len, &field_varint)) {
    if (field_num == 1 && wire_type == PB_WIRE_LENGTH_DELIMITED) {
      current_gid = field_data;
      current_gid_len = field_len;
      break;
    }
  }

  /* Second pass: extract files or recurse */
  pos = 0;
  while (pb_read_field (track_data, track_len, &pos, &field_num, &wire_type,
                        &field_data, &field_len, &field_varint)) {
    if (field_num == 12 && wire_type == PB_WIRE_LENGTH_DELIMITED) {
      const guint8 *file_id_data = NULL;
      gsize file_id_len = 0;
      guint64 format = 0;

      pb_find_bytes_field (field_data, field_len, 1, &file_id_data, &file_id_len);
      pb_find_varint_field (field_data, field_len, 2, &format);

      if (file_id_data && file_id_len == 20) {
        SpclientAudioFile af;
        memset (&af, 0, sizeof(af));
        memcpy (af.file_id, file_id_data, 20);
        if (current_gid && current_gid_len == 16)
          memcpy (af.track_gid, current_gid, 16);
        af.format = format;
        g_array_append_val (files, af);
      }
    } else if (field_num == 13 && wire_type == PB_WIRE_LENGTH_DELIMITED) {
      extract_audio_files_from_track (field_data, field_len, current_gid, current_gid_len, files);
    }
  }
}

static void
on_track_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  TrackRequestClosure *cl  = user_data;
  g_autoptr(GError)    err = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  if (!bytes) {
    if (cl->callback) cl->callback (NULL, 0, err, cl->user_data);
    g_free (cl);
    return;
  }

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);

  const guint8 *ext_meta_data;
  gsize ext_meta_len;
  if (!pb_find_bytes_field (data, len, 2, &ext_meta_data, &ext_meta_len)) {
    g_autofree gchar *hex = bytes_to_hex (data, len);
    g_message ("Failed at parsing BatchedExtensionResponse (field 2). raw hex: %s", hex);
    goto parse_error;
  }

  const guint8 *ext_data1_data;
  gsize ext_data1_len;
  if (!pb_find_bytes_field (ext_meta_data, ext_meta_len, 3, &ext_data1_data, &ext_data1_len)) {
    goto parse_error;
  }

  const guint8 *ext_data2_data;
  gsize ext_data2_len;
  if (!pb_find_bytes_field (ext_data1_data, ext_data1_len, 3, &ext_data2_data, &ext_data2_len)) {
    goto parse_error;
  }

  const guint8 *any_val_data;
  gsize any_val_len;
  if (!pb_find_bytes_field (ext_data2_data, ext_data2_len, 2, &any_val_data, &any_val_len)) {
    goto parse_error;
  }

  GArray *files = g_array_new (FALSE, TRUE, sizeof (SpclientAudioFile));
  extract_audio_files_from_track (any_val_data, any_val_len, NULL, 0, files);

  if (files->len == 0) {
    g_array_free (files, TRUE);
    {
      g_autofree gchar *hex = bytes_to_hex (data, len);
      g_message ("Failed at parsing. raw hex: %s", hex);
      goto parse_error;
    }
  }

  g_message ("spclient: track has %u audio file(s)", files->len);
  if (cl->callback) cl->callback ((SpclientAudioFile *) files->data, files->len, NULL, cl->user_data);

  g_array_free (files, TRUE);
  g_bytes_unref (bytes);
  g_free (cl);
  return;

parse_error:
  {
    GError *parse_err = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
      "failed to parse extended-metadata response (missing or malformed Track proto)");
    g_warning ("spclient: %s", parse_err->message);
    if (cl->callback) cl->callback (NULL, 0, parse_err, cl->user_data);
    g_error_free (parse_err);
    g_bytes_unref (bytes);
    g_free (cl);
  }
}

void
spotifygtk_spclient_get_track_metadata (SpotifySpclient      *self,
                                        const gchar          *track_uri,
                                        const gchar          *bearer_token,
                                        const gchar          *client_token,
                                        SpclientTrackCallback callback,
                                        gpointer              user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_SPCLIENT (self));

  g_autofree gchar *url = g_strdup_printf ("https://%s/extended-metadata/v0/extended-metadata",
                                           SPCLIENT_FALLBACK_HOST);

  GByteArray *req_buf = g_byte_array_new ();
  
  /* ExtensionQuery: extension_kind (1) = TRACK_V4 (10) */
  GByteArray *query_buf = g_byte_array_new ();
  pb_write_varint_field (query_buf, 1, 10);
  
  /* EntityRequest: entity_uri (1) = track_uri, query (2) = query_buf */
  GByteArray *entity_req_buf = g_byte_array_new ();
  pb_write_bytes_field (entity_req_buf, 1, (const guint8 *) track_uri, strlen (track_uri));
  pb_write_message_field (entity_req_buf, 2, query_buf->data, query_buf->len);
  
  /* BatchedEntityRequest: entity_request (2) = entity_req_buf */
  pb_write_message_field (req_buf, 2, entity_req_buf->data, entity_req_buf->len);
  
  g_byte_array_free (query_buf, TRUE);
  g_byte_array_free (entity_req_buf, TRUE);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);

  g_autofree gchar *auth_hdr = g_strdup_printf ("Bearer %s", bearer_token);
  soup_message_headers_replace (headers, "Authorization", auth_hdr);
  soup_message_headers_replace (headers, "Content-Type", "application/x-protobuf");
  soup_message_headers_replace (headers, "Accept", "application/x-protobuf");
  if (client_token && *client_token)
    soup_message_headers_replace (headers, "Client-Token", client_token);

  GBytes *body_bytes = g_byte_array_free_to_bytes (req_buf);
  soup_message_set_request_body_from_bytes (msg, "application/x-protobuf", body_bytes);
  g_bytes_unref (body_bytes);

  TrackRequestClosure *cl = g_new0 (TrackRequestClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, self->cancellable,
                                    on_track_response, cl);
  g_object_unref (msg);
}

void
spotifygtk_spclient_get_audio_storage (SpotifySpclient *self,
                                       const guint8 *file_id, gsize file_id_len,
                                       const gchar *bearer_token, const gchar *client_token,
                                       SpclientStorageCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_SPCLIENT (self));

  g_autofree gchar *file_id_hex = bytes_to_hex (file_id, file_id_len);
  g_autofree gchar *url = g_strdup_printf ("https://%s/storage-resolve/files/audio/interactive/%s",
                                           SPCLIENT_FALLBACK_HOST, file_id_hex);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);

  g_autofree gchar *auth_hdr = g_strdup_printf ("Bearer %s", bearer_token);
  soup_message_headers_replace (headers, "Authorization", auth_hdr);
  soup_message_headers_replace (headers, "Accept", "application/x-protobuf");
  if (client_token && *client_token)
    soup_message_headers_replace (headers, "Client-Token", client_token);

  RequestClosure *cl = g_new0 (RequestClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, self->cancellable,
                                    on_response, cl);
  g_object_unref (msg);
}

static void
spotifygtk_spclient_dispose (GObject *object)
{
  SpotifySpclient *self = SPOTIFYGTK_SPCLIENT (object);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
  G_OBJECT_CLASS (spotifygtk_spclient_parent_class)->dispose (object);
}

static void
spotifygtk_spclient_class_init (SpotifySpclientClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_spclient_dispose;
}

static void
spotifygtk_spclient_init (SpotifySpclient *self)
{
  /*
   * Connection concurrency, overridable so it can be measured rather than
   * argued about -- the same knob cover_loader.c carries, for the same reason.
   * Every context resolve goes to one host, so whatever libsoup defaults to is
   * the ceiling on how many playlists can be described at once.
   */
  const gchar *conns_env = g_getenv ("SPOTIFY_SPCLIENT_CONNS");
  guint conns = conns_env ? (guint) MAX (1, atoi (conns_env)) : 8;

  self->session = soup_session_new_with_options (
    "user-agent", "spotify-native/" APP_VERSION,
    "max-conns-per-host", conns,
    "max-conns", MAX (conns, 24),
    NULL);
  g_message ("spclient: session using %u connections per host", conns);
}

SpotifySpclient *
spotifygtk_spclient_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_SPCLIENT, NULL);
}

void spotifygtk_spclient_set_cancellable (SpotifySpclient *self, GCancellable *cancellable)
{ g_set_object (&self->cancellable, cancellable); }

/* ── Batched display metadata ────────────────────────────────────────────── */

typedef struct {
  SpclientBatchCallback callback;
  gpointer              user_data;
} BatchRequestClosure;

/* extended_metadata.proto / entity_extension_data.proto field numbers */
#define BER_EXTENDED_METADATA   2   /* BatchedExtensionResponse.extended_metadata (repeated) */
#define EEDA_EXTENSION_DATA     3   /* EntityExtensionDataArray.extension_data   (repeated) */
#define EED_ENTITY_URI          2   /* EntityExtensionData.entity_uri */
#define EED_EXTENSION_DATA      3   /* EntityExtensionData.extension_data (google.protobuf.Any) */
#define ANY_VALUE               2   /* google.protobuf.Any.value */

static void
on_batch_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  BatchRequestClosure *cl  = user_data;
  g_autoptr(GError)    err = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  if (!bytes) {
    if (cl->callback) cl->callback (NULL, 0, err, cl->user_data);
    g_free (cl);
    return;
  }

  gsize         len  = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);

  GArray *out = g_array_new (FALSE, TRUE, sizeof (SpclientTrackInfo));

  /* Both extended_metadata and extension_data are repeated, so neither can
   * use pb_find_bytes_field (first match only) -- walk them with
   * pb_read_field. One request for N tracks comes back as one array with N
   * entries, but a server splitting them across arrays would still parse. */
  gsize         outer_pos = 0;
  guint32       field_num;
  PbWireType    wire_type;
  const guint8 *field_data;
  gsize         field_len;
  guint64       field_varint;

  while (pb_read_field (data, len, &outer_pos, &field_num, &wire_type,
                        &field_data, &field_len, &field_varint)) {
    if (field_num != BER_EXTENDED_METADATA || wire_type != PB_WIRE_LENGTH_DELIMITED)
      continue;

    const guint8 *array_data = field_data;
    gsize         array_len  = field_len;

    gsize         inner_pos = 0;
    guint32       inner_field;
    PbWireType    inner_wire;
    const guint8 *inner_data;
    gsize         inner_len;
    guint64       inner_varint;

    while (pb_read_field (array_data, array_len, &inner_pos, &inner_field, &inner_wire,
                          &inner_data, &inner_len, &inner_varint)) {
      if (inner_field != EEDA_EXTENSION_DATA || inner_wire != PB_WIRE_LENGTH_DELIMITED)
        continue;

      /* EntityExtensionData: entity_uri (2), extension_data (3 = Any) */
      const guint8 *uri_data = NULL;
      gsize         uri_len  = 0;
      gboolean have_uri = pb_find_bytes_field (inner_data, inner_len,
                                               EED_ENTITY_URI, &uri_data, &uri_len);

      const guint8 *any_data = NULL;
      gsize         any_len  = 0;
      if (!pb_find_bytes_field (inner_data, inner_len, EED_EXTENSION_DATA,
                                &any_data, &any_len))
        continue;

      const guint8 *track_data = NULL;
      gsize         track_len  = 0;
      if (!pb_find_bytes_field (any_data, any_len, ANY_VALUE, &track_data, &track_len))
        continue;

      SpclientTrackInfo info;
      memset (&info, 0, sizeof info);

      if (!spotifygtk_track_meta_parse (track_data, track_len, &info.meta))
        continue;

      info.entity_uri = have_uri ? g_strndup ((const gchar *) uri_data, uri_len) : NULL;
      g_array_append_val (out, info);
    }
  }

  if (out->len == 0) {
    GError *empty = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
      "extended-metadata batch response contained no parseable tracks");
    g_warning ("spclient: %s", empty->message);
    if (cl->callback) cl->callback (NULL, 0, empty, cl->user_data);
    g_error_free (empty);
  } else {
    g_message ("spclient: batch resolved %u track(s)", out->len);
    if (cl->callback)
      cl->callback ((const SpclientTrackInfo *) out->data, out->len, NULL, cl->user_data);
  }

  for (guint i = 0; i < out->len; i++) {
    SpclientTrackInfo *info = &g_array_index (out, SpclientTrackInfo, i);
    g_free (info->entity_uri);
    spotifygtk_track_meta_clear (&info->meta);
  }
  g_array_free (out, TRUE);

  g_bytes_unref (bytes);
  g_free (cl);
}

void
spotifygtk_spclient_get_tracks_metadata (SpotifySpclient      *self,
                                         const gchar *const   *track_uris,
                                         guint                 n_uris,
                                         const gchar          *bearer_token,
                                         const gchar          *client_token,
                                         SpclientBatchCallback callback,
                                         gpointer              user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_SPCLIENT (self));
  g_return_if_fail (track_uris != NULL || n_uris == 0);

  if (n_uris == 0) {
    if (callback) {
      GError *none = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                          "no track URIs requested");
      callback (NULL, 0, none, user_data);
      g_error_free (none);
    }
    return;
  }

  g_autofree gchar *url = g_strdup_printf ("https://%s/extended-metadata/v0/extended-metadata",
                                           SPCLIENT_FALLBACK_HOST);

  GByteArray *req_buf = g_byte_array_new ();

  for (guint i = 0; i < n_uris; i++) {
    if (!track_uris[i])
      continue;

    /* ExtensionQuery: extension_kind (1) = TRACK_V4 (10) */
    GByteArray *query_buf = g_byte_array_new ();
    pb_write_varint_field (query_buf, 1, 10);

    /* EntityRequest: entity_uri (1), query (2) */
    GByteArray *entity_req_buf = g_byte_array_new ();
    pb_write_bytes_field (entity_req_buf, 1,
                          (const guint8 *) track_uris[i], strlen (track_uris[i]));
    pb_write_message_field (entity_req_buf, 2, query_buf->data, query_buf->len);

    /* BatchedEntityRequest.entity_request (2) -- repeated, so this tag
     * simply repeats once per track. */
    pb_write_message_field (req_buf, 2, entity_req_buf->data, entity_req_buf->len);

    g_byte_array_free (query_buf, TRUE);
    g_byte_array_free (entity_req_buf, TRUE);
  }

  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);

  g_autofree gchar *auth_hdr = g_strdup_printf ("Bearer %s", bearer_token ? bearer_token : "");
  soup_message_headers_replace (headers, "Authorization", auth_hdr);
  soup_message_headers_replace (headers, "Content-Type", "application/x-protobuf");
  soup_message_headers_replace (headers, "Accept", "application/x-protobuf");
  if (client_token && *client_token)
    soup_message_headers_replace (headers, "Client-Token", client_token);

  GBytes *body_bytes = g_byte_array_free_to_bytes (req_buf);
  soup_message_set_request_body_from_bytes (msg, "application/x-protobuf", body_bytes);
  g_bytes_unref (body_bytes);

  BatchRequestClosure *cl = g_new0 (BatchRequestClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, self->cancellable,
                                    on_batch_response, cl);
  g_object_unref (msg);
}

/* ── Context resolution ──────────────────────────────────────────────────── */

typedef struct {
  SpclientContextCallback callback;
  gpointer                user_data;
} ContextRequestClosure;

gchar *
spotifygtk_spclient_build_search_uri (const gchar *query)
{
  if (!query || !*query)
    return NULL;

  /* Spaces become '+' (spclient.rs: "whitespaces are replaced with +").
   * Everything else is percent-escaped: a query is arbitrary user input
   * and ':' or '/' left raw would change how the URI parses and how the
   * request path is built. '+' itself must be reserved from the escape
   * set or the separator would come back percent-encoded. */
  g_autofree gchar *collapsed = g_strdup (query);
  for (gchar *p = collapsed; *p; p++) {
    if (g_ascii_isspace (*p))
      *p = '+';
  }

  g_autofree gchar *escaped = g_uri_escape_string (collapsed, "+", FALSE);
  return g_strdup_printf ("spotify:search:%s", escaped);
}

gchar *
spotifygtk_spclient_build_collection_uri (const gchar *user_id)
{
  if (!user_id || !*user_id)
    return NULL;

  g_autofree gchar *escaped = g_uri_escape_string (user_id, NULL, FALSE);
  return g_strdup_printf ("spotify:user:%s:collection", escaped);
}

static void
on_context_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ContextRequestClosure *cl  = user_data;
  g_autoptr(GError)      err = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  if (!bytes) {
    if (cl->callback) cl->callback (NULL, err, cl->user_data);
    g_free (cl);
    return;
  }

  gsize        len  = 0;
  const gchar *body = g_bytes_get_data (bytes, &len);

  /* An empty body is a documented outcome, not a parse failure:
   * librespot's get_context treats it as SpClientError::NoData. */
  if (len == 0) {
    GError *no_data = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
      "context-resolve returned an empty body (no data for this URI)");
    if (cl->callback) cl->callback (NULL, no_data, cl->user_data);
    g_error_free (no_data);
    g_bytes_unref (bytes);
    g_free (cl);
    return;
  }

  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GError) parse_err = NULL;

  if (!json_parser_load_from_data (parser, body, (gssize) len, &parse_err)) {
    /* Surface a snippet: an auth failure here arrives as an HTML or
     * plain-text error page, and "invalid JSON" alone would hide that. */
    g_autofree gchar *snippet = g_strndup (body, MIN (len, 200));
    GError *bad = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
      "context-resolve response was not JSON (%s): %s",
      parse_err->message, snippet);
    g_warning ("spclient: %s", bad->message);
    if (cl->callback) cl->callback (NULL, bad, cl->user_data);
    g_error_free (bad);
    g_bytes_unref (bytes);
    g_free (cl);
    return;
  }

  JsonNode *root = json_parser_get_root (parser);
  if (cl->callback)
    cl->callback (root ? json_node_copy (root) : NULL, NULL, cl->user_data);

  g_bytes_unref (bytes);
  g_free (cl);
}


/*
 * Artist portrait, over the same batch endpoint the track metadata uses.
 *
 * Field numbers all recovered from the shipped client's own descriptors rather
 * than assumed: ExtensionKind.ARTIST_V4 = 8 (the enum there also gives
 * TRACK_V4 = 10, which is the constant this file already used, so the reading
 * is self-checking), and Artist.portrait_group = 17 -- the same number Album
 * gives cover_group, which is why the image parsing below is identical to the
 * album path.
 */
#define EXTENSION_KIND_ARTIST_V4   8
#define ARTIST_FIELD_PORTRAIT_GROUP 17
#define ARTIST_FIELD_PORTRAIT       11   /* repeated Image, older shape */

typedef struct {
  SpclientArtistCallback callback;
  gpointer               user_data;
} ArtistClosure;

/* Widest image in an ImageGroup, as hex. The hero is drawn large, so unlike a
 * row thumbnail it genuinely wants the biggest variant. */
static gchar *
dup_widest_image (const guint8 *group, gsize len)
{
  gsize         pos = 0;
  guint32       fnum;
  PbWireType    wtype;
  const guint8 *fdata;
  gsize         flen;
  guint64       fvarint;

  gchar  *best   = NULL;
  gint64  best_w = -1;

  while (pb_read_field (group, len, &pos, &fnum, &wtype, &fdata, &flen, &fvarint)) {
    if (fnum != 1 || wtype != PB_WIRE_LENGTH_DELIMITED)   /* ImageGroup.image */
      continue;

    const guint8 *id = NULL;
    gsize         id_len = 0;
    if (!pb_find_bytes_field (fdata, flen, 1, &id, &id_len))   /* Image.file_id */
      continue;

    guint64 raw = 0;
    gint64  w   = 0;
    if (pb_find_varint_field (fdata, flen, 3, &raw))           /* Image.width */
      w = (gint64) ((raw >> 1) ^ (~(raw & 1) + 1));

    if (w > best_w || best == NULL) {
      g_free (best);
      GString *hex = g_string_sized_new (id_len * 2);
      for (gsize i = 0; i < id_len; i++)
        g_string_append_printf (hex, "%02x", id[i]);
      best = g_string_free (hex, FALSE);
      best_w = w;
    }
  }
  return best;
}

static void
on_artist_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ArtistClosure *cl = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  if (!body) {
    if (cl->callback) cl->callback (NULL, error, cl->user_data);
    g_free (cl);
    return;
  }

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (body, &len);

  gsize         pos = 0;
  guint32       fnum;
  PbWireType    wtype;
  const guint8 *fdata;
  gsize         flen;
  guint64       fvarint;
  g_autofree gchar *cover = NULL;

  /* Same envelope as the track batch: extended_metadata -> extension_data ->
   * Any.value, with an Artist inside instead of a Track. */
  while (!cover && pb_read_field (data, len, &pos, &fnum, &wtype, &fdata, &flen, &fvarint)) {
    if (fnum != BER_EXTENDED_METADATA || wtype != PB_WIRE_LENGTH_DELIMITED)
      continue;

    gsize         ipos = 0;
    guint32       ifnum;
    PbWireType    iwtype;
    const guint8 *idata;
    gsize         ilen;
    guint64       ivarint;

    while (!cover && pb_read_field (fdata, flen, &ipos, &ifnum, &iwtype,
                                    &idata, &ilen, &ivarint)) {
      if (ifnum != EEDA_EXTENSION_DATA || iwtype != PB_WIRE_LENGTH_DELIMITED)
        continue;

      const guint8 *any = NULL; gsize any_len = 0;
      if (!pb_find_bytes_field (idata, ilen, EED_EXTENSION_DATA, &any, &any_len))
        continue;
      const guint8 *artist = NULL; gsize artist_len = 0;
      if (!pb_find_bytes_field (any, any_len, ANY_VALUE, &artist, &artist_len))
        continue;

      const guint8 *grp = NULL; gsize grp_len = 0;
      if (pb_find_bytes_field (artist, artist_len, ARTIST_FIELD_PORTRAIT_GROUP,
                               &grp, &grp_len))
        cover = dup_widest_image (grp, grp_len);

      /* Some artists carry the older repeated Image instead of the group. */
      if (!cover && pb_find_bytes_field (artist, artist_len, ARTIST_FIELD_PORTRAIT,
                                         &grp, &grp_len)) {
        const guint8 *id = NULL; gsize id_len = 0;
        if (pb_find_bytes_field (grp, grp_len, 1, &id, &id_len)) {
          GString *hex = g_string_sized_new (id_len * 2);
          for (gsize i = 0; i < id_len; i++)
            g_string_append_printf (hex, "%02x", id[i]);
          cover = g_string_free (hex, FALSE);
        }
      }
    }
  }

  if (cl->callback)
    cl->callback (cover, NULL, cl->user_data);
  g_free (cl);
}


/*
 * Artist header, over pathfinder.
 *
 * Operation name and persisted-query hash both read out of the shipped
 * client's bundle rather than guessed:
 *
 *   new c.l("queryArtistOverview", "query",
 *           "1ac33dda...737c72", null)
 *
 * A persisted query means the document itself is never sent -- the server
 * already has it and is addressed by that hash -- so this cannot be adjusted
 * to ask for more without a hash that matches whatever it was changed to.
 */
#define PATHFINDER_HOST      "api-partner.spotify.com"
#define ARTIST_OVERVIEW_OP   "queryArtistOverview"
#define ARTIST_OVERVIEW_HASH \
  "1ac33ddab5d39a3a9c27802774e6d78b9405cc188c6f75aed007df2a32737c72"

/* i.scdn.co URLs end in the image id the cover loader wants. */
static gchar *
dup_image_id_from_url (const gchar *url)
{
  if (!url || !*url)
    return NULL;
  const gchar *last = strrchr (url, '/');
  const gchar *id   = last ? last + 1 : url;
  if (!*id)
    return NULL;
  for (const gchar *p = id; *p; p++)
    if (!g_ascii_isxdigit (*p))
      return NULL;
  return g_strdup (id);
}

static void
on_artist_header_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ArtistClosure *cl = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  if (!body) {
    if (cl->callback) cl->callback (NULL, error, cl->user_data);
    g_free (cl);
    return;
  }

  gsize len = 0;
  const gchar *json = g_bytes_get_data (body, &len);

  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autofree gchar *cover = NULL;

  if (json_parser_load_from_data (parser, json, (gssize) len, NULL)) {
    JsonNode *root = json_parser_get_root (parser);
    if (root && JSON_NODE_HOLDS_OBJECT (root)) {
      JsonObject *o = json_node_get_object (root);
      /* data.artistUnion.visuals.headerImage.sources[0].url */
      if (json_object_has_member (o, "data"))
        o = json_object_get_object_member (o, "data");
      if (o && json_object_has_member (o, "artistUnion"))
        o = json_object_get_object_member (o, "artistUnion");
      if (o && json_object_has_member (o, "visuals"))
        o = json_object_get_object_member (o, "visuals");
      if (o && json_object_has_member (o, "headerImage") &&
          !JSON_NODE_HOLDS_NULL (json_object_get_member (o, "headerImage")))
        o = json_object_get_object_member (o, "headerImage");
      else
        o = NULL;

      /* Some responses nest the image under `data`, as ImageV2 does. */
      if (o && json_object_has_member (o, "data"))
        o = json_object_get_object_member (o, "data");

      JsonArray *sources = (o && json_object_has_member (o, "sources"))
        ? json_object_get_array_member (o, "sources") : NULL;

      /* Widest first: the banner is drawn large. */
      gint best_w = -1;
      for (guint i = 0; sources && i < json_array_get_length (sources); i++) {
        JsonObject *src = json_array_get_object_element (sources, i);
        if (!src || !json_object_has_member (src, "url"))
          continue;
        gint w = json_object_has_member (src, "width")
          ? (gint) json_object_get_int_member (src, "width") : 0;
        if (w > best_w) {
          g_autofree gchar *id =
            dup_image_id_from_url (json_object_get_string_member (src, "url"));
          if (id) {
            g_free (cover);
            cover = g_steal_pointer (&id);
            best_w = w;
          }
        }
      }
    }
  }

  if (cl->callback)
    cl->callback (cover, NULL, cl->user_data);
  g_free (cl);
}

void
spotifygtk_spclient_get_artist_header (SpotifySpclient        *self,
                                       const gchar            *artist_uri,
                                       const gchar            *bearer_token,
                                       const gchar            *client_token,
                                       SpclientArtistCallback  callback,
                                       gpointer                user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_SPCLIENT (self));

  if (!artist_uri || !*artist_uri) {
    if (callback) callback (NULL, NULL, user_data);
    return;
  }

  g_autofree gchar *variables = g_strdup_printf (
    "{\"uri\":\"%s\",\"locale\":\"\",\"includePrerelease\":true}", artist_uri);
  g_autofree gchar *extensions = g_strdup_printf (
    "{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"%s\"}}",
    ARTIST_OVERVIEW_HASH);

  g_autofree gchar *var_esc = g_uri_escape_string (variables, NULL, TRUE);
  g_autofree gchar *ext_esc = g_uri_escape_string (extensions, NULL, TRUE);

  g_autofree gchar *url = g_strdup_printf (
    "https://%s/pathfinder/v1/query?operationName=%s&variables=%s&extensions=%s",
    PATHFINDER_HOST, ARTIST_OVERVIEW_OP, var_esc, ext_esc);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);

  g_autofree gchar *auth_hdr = g_strdup_printf ("Bearer %s", bearer_token ? bearer_token : "");
  soup_message_headers_replace (headers, "Authorization", auth_hdr);
  soup_message_headers_replace (headers, "Accept", "application/json");
  if (client_token && *client_token)
    soup_message_headers_replace (headers, "Client-Token", client_token);

  ArtistClosure *cl = g_new0 (ArtistClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    self->cancellable, on_artist_header_response, cl);
  g_object_unref (msg);
}

void
spotifygtk_spclient_get_artist_portrait (SpotifySpclient        *self,
                                         const gchar            *artist_uri,
                                         const gchar            *bearer_token,
                                         const gchar            *client_token,
                                         SpclientArtistCallback  callback,
                                         gpointer                user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_SPCLIENT (self));

  if (!artist_uri || !*artist_uri) {
    if (callback) callback (NULL, NULL, user_data);
    return;
  }

  g_autofree gchar *url = g_strdup_printf (
    "https://%s/extended-metadata/v0/extended-metadata", SPCLIENT_FALLBACK_HOST);

  GByteArray *req_buf = g_byte_array_new ();

  GByteArray *query_buf = g_byte_array_new ();
  pb_write_varint_field (query_buf, 1, EXTENSION_KIND_ARTIST_V4);

  GByteArray *entity_req_buf = g_byte_array_new ();
  pb_write_bytes_field (entity_req_buf, 1,
                        (const guint8 *) artist_uri, strlen (artist_uri));
  pb_write_message_field (entity_req_buf, 2, query_buf->data, query_buf->len);
  pb_write_message_field (req_buf, 2, entity_req_buf->data, entity_req_buf->len);

  g_byte_array_free (query_buf, TRUE);
  g_byte_array_free (entity_req_buf, TRUE);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  g_autofree gchar *auth_hdr = g_strdup_printf ("Bearer %s", bearer_token ? bearer_token : "");
  soup_message_headers_replace (headers, "Authorization", auth_hdr);
  soup_message_headers_replace (headers, "Content-Type", "application/x-protobuf");
  soup_message_headers_replace (headers, "Accept", "application/x-protobuf");
  if (client_token && *client_token)
    soup_message_headers_replace (headers, "Client-Token", client_token);

  GBytes *body_bytes = g_byte_array_free_to_bytes (req_buf);
  soup_message_set_request_body_from_bytes (msg, "application/x-protobuf", body_bytes);
  g_bytes_unref (body_bytes);

  ArtistClosure *cl = g_new0 (ArtistClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    self->cancellable, on_artist_response, cl);
  g_object_unref (msg);
}

void
spotifygtk_spclient_get_context (SpotifySpclient        *self,
                                 const gchar            *context_uri,
                                 const gchar            *bearer_token,
                                 const gchar            *client_token,
                                 SpclientContextCallback callback,
                                 gpointer                user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_SPCLIENT (self));
  g_return_if_fail (context_uri != NULL);

  /* The URI is a path segment, so it must be escaped as one. ':' is left
   * unreserved because the spotify:...:... form depends on it and it is
   * legal in a path segment per RFC 3986. */
  g_autofree gchar *escaped_uri = g_uri_escape_string (context_uri, ":", FALSE);
  g_autofree gchar *url = g_strdup_printf ("https://%s/context-resolve/v1/%s",
                                           SPCLIENT_FALLBACK_HOST, escaped_uri);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);

  g_autofree gchar *auth_hdr = g_strdup_printf ("Bearer %s", bearer_token ? bearer_token : "");
  soup_message_headers_replace (headers, "Authorization", auth_hdr);
  soup_message_headers_replace (headers, "Accept", "application/json");
  if (client_token)
    soup_message_headers_replace (headers, "Client-Token", client_token);

  ContextRequestClosure *cl = g_new0 (ContextRequestClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, self->cancellable,
                                    on_context_response, cl);
  g_object_unref (msg);
}

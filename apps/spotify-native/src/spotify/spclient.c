/*
 * spclient.c — HTTPS client for track metadata / CDN URL resolution.
 * See spclient.h for scope and the apresolve simplification note.
 */

#include "config.h"
#include "spclient.h"

#include <stdlib.h>
#include "protobuf_min.h"
#include "track_meta.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define PATHFINDER_URL \
  "https://api-partner.spotify.com/pathfinder/v1/query"
/* Required: api-partner refuses the same authenticated request when it does
 * not look like a browser. Kept on a dedicated SoupSession below. */
#define PATHFINDER_UA \
  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) " \
  "Chrome/126.0.0.0 Safari/537.36"

struct _SpotifySpclient {
  GObject      parent_instance;
  SoupSession *session;
  /*
   * A second session, for pathfinder only.
   *
   * SoupSession:user-agent is applied to every message it queues, overwriting
   * whatever the message already carried -- so setting a browser UA on the
   * message was silently undone, and the request went on 403'ing. pathfinder
   * refuses anything that does not look like a browser, so it needs a session
   * whose UA is one.
   */
  SoupSession *gql_session;
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
    /*
     * Parsed fine; the server simply has no playable file for this track.
     *
     * This used to dump the whole response as hex and report a malformed
     * Track proto, which sent every reading of it -- including mine -- after
     * a parser bug that was not there. Confirmed against the other metadata
     * path: hm://metadata/4/track/<gid> returns the identical 563-byte Track
     * with no AudioFile either, so the absence is the server's answer and not
     * something this client can parse its way out of.
     *
     * NOT_FOUND rather than FAILED so the caller can tell "this track cannot
     * play" from "this attempt went wrong", and skip instead of retrying.
     */
    g_array_free (files, TRUE);
    GError *gone = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
      "track is unavailable: the server returned no playable audio files");
    g_message ("spclient: %s", gone->message);
    if (cl->callback) cl->callback (NULL, 0, gone, cl->user_data);
    g_error_free (gone);
    g_bytes_unref (bytes);
    g_free (cl);
    return;
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
  g_clear_object (&self->gql_session);
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

/* The old context-resolve search form is a 20-track playback context. The
 * shipped desktop client uses this persisted searchTracks query, which pages
 * in 50-track slices. Keep the legacy context as a fallback in case Spotify
 * rotates the hash before this client is updated. */
#define SEARCH_TRACKS_HASH \
  "59ee4a659c32e9ad894a71308207594a65ba67bb6b632b183abe97303a51fa55"
#define SEARCH_PAGE_SIZE 50
#define SEARCH_RESULT_TARGET 300

typedef struct {
  SpotifySpclient        *spclient;      /* borrowed; owner keeps requests alive */
  SpclientContextCallback callback;
  gpointer                user_data;
  gchar                   *context_uri;
  gchar                   *query;
  gchar                   *bearer_token;
  gchar                   *client_token;
  guint                    offset;
  GPtrArray               *uris;         /* owned strings */
  GHashTable              *seen;         /* borrowed keys from uris */
  SoupMessage             *message;      /* current request, owned */
} SearchContextClosure;

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

static void
request_context_resolve (SpotifySpclient *self, const gchar *context_uri,
                         const gchar *bearer_token, const gchar *client_token,
                         SpclientContextCallback callback, gpointer user_data)
{
  /* The URI is a path segment, so it must be escaped as one. ':' is left
   * unreserved because the spotify:...:... form depends on it and it is
   * legal in a path segment per RFC 3986. */
  g_autofree gchar *escaped_uri = g_uri_escape_string (context_uri, ":", FALSE);
  g_autofree gchar *url = g_strdup_printf ("https://%s/context-resolve/v1/%s",
                                           SPCLIENT_FALLBACK_HOST, escaped_uri);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  g_autofree gchar *auth_hdr =
    g_strdup_printf ("Bearer %s", bearer_token ? bearer_token : "");
  soup_message_headers_replace (headers, "Authorization", auth_hdr);
  soup_message_headers_replace (headers, "Accept", "application/json");
  if (client_token && *client_token)
    soup_message_headers_replace (headers, "Client-Token", client_token);

  ContextRequestClosure *cl = g_new0 (ContextRequestClosure, 1);
  cl->callback = callback;
  cl->user_data = user_data;
  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    self->cancellable, on_context_response, cl);
  g_object_unref (msg);
}

static void
search_context_closure_free (SearchContextClosure *cl)
{
  g_clear_object (&cl->message);
  /* `seen` borrows its keys from `uris`, so discard the index first. */
  g_clear_pointer (&cl->seen, g_hash_table_unref);
  g_clear_pointer (&cl->uris, g_ptr_array_unref);
  g_free (cl->context_uri);
  g_free (cl->query);
  g_free (cl->bearer_token);
  g_free (cl->client_token);
  g_free (cl);
}

static void search_context_request_page (SearchContextClosure *cl);

static void
search_context_complete (SearchContextClosure *cl)
{
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "pages");
  json_builder_begin_array (builder);
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "tracks");
  json_builder_begin_array (builder);
  for (guint i = 0; i < cl->uris->len; i++) {
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "uri");
    json_builder_add_string_value (builder, g_ptr_array_index (cl->uris, i));
    json_builder_end_object (builder);
  }
  json_builder_end_array (builder);
  json_builder_end_object (builder);
  json_builder_end_array (builder);
  json_builder_set_member_name (builder, "uri");
  json_builder_add_string_value (builder, cl->context_uri);
  json_builder_end_object (builder);

  JsonNode *context = json_builder_get_root (builder);
  g_message ("spclient: desktop search returned %u track URI(s)", cl->uris->len);
  if (cl->callback)
    cl->callback (context, NULL, cl->user_data);
  else
    json_node_unref (context);
  search_context_closure_free (cl);
}

static void
search_context_fallback (SearchContextClosure *cl, const gchar *reason)
{
  g_warning ("spclient: expanded desktop search unavailable (%s); falling "
             "back to the 20-track search context", reason ? reason : "unknown");
  request_context_resolve (cl->spclient, cl->context_uri, cl->bearer_token,
                           cl->client_token, cl->callback, cl->user_data);
  search_context_closure_free (cl);
}

static JsonObject *
json_object_child (JsonObject *parent, const gchar *member)
{
  if (!parent || !json_object_has_member (parent, member))
    return NULL;
  JsonNode *node = json_object_get_member (parent, member);
  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

static void
on_search_context_page (GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
  SearchContextClosure *cl = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  guint status = cl->message ? soup_message_get_status (cl->message) : 0;
  g_clear_object (&cl->message);

  if (!body || status < 200 || status >= 300) {
    if (cl->uris->len > 0) {
      search_context_complete (cl);       /* a later page failed; keep page 1 */
      return;
    }
    g_autofree gchar *why = g_strdup_printf ("HTTP %u%s%s", status,
      error ? ": " : "", error ? error->message : "");
    search_context_fallback (cl, why);
    return;
  }

  gsize len = 0;
  const gchar *json = g_bytes_get_data (body, &len);
  g_autoptr(JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json, (gssize) len, &error)) {
    if (cl->uris->len > 0)
      search_context_complete (cl);
    else
      search_context_fallback (cl, error ? error->message : "invalid JSON");
    return;
  }

  JsonNode *root_node = json_parser_get_root (parser);
  JsonObject *root = root_node && JSON_NODE_HOLDS_OBJECT (root_node)
    ? json_node_get_object (root_node) : NULL;
  JsonObject *data = json_object_child (root, "data");
  JsonObject *search = json_object_child (data, "searchV2");
  JsonObject *tracks = json_object_child (search, "tracksV2");
  JsonArray *items = tracks && json_object_has_member (tracks, "items")
    ? json_object_get_array_member (tracks, "items") : NULL;

  for (guint i = 0; items && i < json_array_get_length (items) &&
                    cl->uris->len < SEARCH_RESULT_TARGET; i++) {
    JsonObject *wrapper = json_array_get_object_element (items, i);
    JsonObject *item = json_object_child (wrapper, "item");
    JsonObject *track = json_object_child (item, "data");
    if (!track || !json_object_has_member (track, "uri"))
      continue;
    const gchar *uri = json_object_get_string_member (track, "uri");
    if (!uri || !g_str_has_prefix (uri, "spotify:track:") ||
        g_hash_table_contains (cl->seen, uri))
      continue;
    gchar *owned = g_strdup (uri);
    g_ptr_array_add (cl->uris, owned);
    g_hash_table_add (cl->seen, owned);
  }

  JsonObject *paging = json_object_child (tracks, "pagingInfo");
  gint64 next = -1;
  if (paging && json_object_has_member (paging, "nextOffset")) {
    JsonNode *next_node = json_object_get_member (paging, "nextOffset");
    if (next_node && !JSON_NODE_HOLDS_NULL (next_node))
      next = json_node_get_int (next_node);
  }

  if (cl->uris->len >= SEARCH_RESULT_TARGET || next < 0 ||
      next <= (gint64) cl->offset) {
    if (cl->uris->len > 0)
      search_context_complete (cl);
    else
      search_context_fallback (cl, "response contained no track results");
    return;
  }

  cl->offset = (guint) next;
  search_context_request_page (cl);
}

static void
search_context_request_page (SearchContextClosure *cl)
{
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
#define ADD_BOOL(name, value) G_STMT_START { \
  json_builder_set_member_name (builder, name); \
  json_builder_add_boolean_value (builder, value); \
} G_STMT_END
#define ADD_INT(name, value) G_STMT_START { \
  json_builder_set_member_name (builder, name); \
  json_builder_add_int_value (builder, value); \
} G_STMT_END
  json_builder_set_member_name (builder, "searchTerm");
  json_builder_add_string_value (builder, cl->query);
  ADD_INT ("offset", cl->offset);
  ADD_INT ("limit", SEARCH_PAGE_SIZE);
  ADD_BOOL ("includeAudiobooks", TRUE);
  ADD_BOOL ("includePreReleases", FALSE);
  ADD_BOOL ("includeAlbumPreReleases", FALSE);
  ADD_BOOL ("includeAuthors", FALSE);
  ADD_BOOL ("includeEpisodeContentRatingsV2", TRUE);
  ADD_INT ("numberOfTopResults", 20);
  json_builder_end_object (builder);
#undef ADD_BOOL
#undef ADD_INT

  g_autoptr(JsonGenerator) generator = json_generator_new ();
  g_autoptr(JsonNode) variables_root = json_builder_get_root (builder);
  json_generator_set_root (generator, variables_root);
  g_autofree gchar *variables = json_generator_to_data (generator, NULL);
  g_autofree gchar *extensions = g_strdup_printf (
    "{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"%s\"}}",
    SEARCH_TRACKS_HASH);
  g_autofree gchar *enc_vars = g_uri_escape_string (variables, NULL, FALSE);
  g_autofree gchar *enc_ext = g_uri_escape_string (extensions, NULL, FALSE);
  g_autofree gchar *url = g_strdup_printf (
    "%s?operationName=searchTracks&variables=%s&extensions=%s",
    PATHFINDER_URL, enc_vars, enc_ext);

  cl->message = soup_message_new (SOUP_METHOD_GET, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (cl->message);
  g_autofree gchar *auth_hdr =
    g_strdup_printf ("Bearer %s", cl->bearer_token ? cl->bearer_token : "");
  soup_message_headers_replace (headers, "Authorization", auth_hdr);
  soup_message_headers_replace (headers, "Accept", "application/json");
  if (cl->client_token && *cl->client_token)
    soup_message_headers_replace (headers, "Client-Token", cl->client_token);

  if (!cl->spclient->gql_session)
    cl->spclient->gql_session = soup_session_new_with_options (
      "user-agent", PATHFINDER_UA, NULL);
  soup_session_send_and_read_async (
    cl->spclient->gql_session, cl->message, G_PRIORITY_DEFAULT,
    cl->spclient->cancellable, on_search_context_page, cl);
}

static void
request_expanded_search (SpotifySpclient *self, const gchar *context_uri,
                         const gchar *bearer_token, const gchar *client_token,
                         SpclientContextCallback callback, gpointer user_data)
{
  const gchar *encoded = context_uri + strlen ("spotify:search:");
  g_autofree gchar *query = g_uri_unescape_string (encoded, NULL);
  if (!query) {
    request_context_resolve (self, context_uri, bearer_token, client_token,
                             callback, user_data);
    return;
  }
  for (gchar *p = query; *p; p++)
    if (*p == '+')
      *p = ' ';

  SearchContextClosure *cl = g_new0 (SearchContextClosure, 1);
  cl->spclient = self;
  cl->callback = callback;
  cl->user_data = user_data;
  cl->context_uri = g_strdup (context_uri);
  cl->query = g_strdup (query);
  cl->bearer_token = g_strdup (bearer_token);
  cl->client_token = g_strdup (client_token);
  cl->uris = g_ptr_array_new_with_free_func (g_free);
  cl->seen = g_hash_table_new (g_str_hash, g_str_equal);
  search_context_request_page (cl);
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
#define ARTIST_FIELD_NAME            2
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
    if (cl->callback) cl->callback (NULL, NULL, error, cl->user_data);
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
  g_autofree gchar *name = NULL;

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

      const guint8 *name_data = NULL; gsize name_len = 0;
      if (pb_find_bytes_field (artist, artist_len, ARTIST_FIELD_NAME,
                               &name_data, &name_len))
        name = g_strndup ((const gchar *) name_data, name_len);

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
    cl->callback (name, cover, NULL, cl->user_data);
  g_free (cl);
}






/* ── Discography ─────────────────────────────────────────────────────────── */
/*
 * Field numbers, all read out of the shipped client's descriptors:
 *
 *   Artist.album_group = 5, single_group = 6, compilation_group = 7
 *   AlbumGroup.album   = 1,  Album.gid = 1
 *   Album.name = 2, type = 4, date = 6, disc = 11, cover_group = 17
 *   Date.year  = 1,  Disc.track = 3,  Track.gid = 1
 *
 * Corroborated against the catalogue rather than trusted: for one artist the
 * groups hold 4 / 73 / 2 entries, and the same artist's discography reports
 * 4 albums, 73 singles and 2 compilations. cover_group = 17 is the number the
 * album path in this file already used, so that much is self-checking.
 */
#define EXTENSION_KIND_ALBUM_V4      9
#define ARTIST_FIELD_ALBUM_GROUP     5
#define ARTIST_FIELD_SINGLE_GROUP    6
#define ARTIST_FIELD_COMPIL_GROUP    7
#define ALBUM_GROUP_ALBUM            1
#define ALBUM_FIELD_GID              1
#define ALBUM_FIELD_NAME             2
#define ALBUM_FIELD_TYPE             4
#define ALBUM_FIELD_DATE             6
#define ALBUM_FIELD_DISC            11
#define ALBUM_FIELD_COVER_GROUP     17
#define DISC_FIELD_TRACK             3
#define TRACK_FIELD_GID              1

/* Walk the batch envelope and hand each entity payload to one of these. */
typedef void (*EntityFunc) (const guint8 *entity, gsize len, gpointer user_data);

typedef struct {
  SpclientReleasesCallback callback;
  gpointer                 user_data;
  EntityFunc               entity_fn;
} ReleasesClosure;

void
spotifygtk_release_free (SpotifyRelease *release)
{
  if (!release)
    return;
  g_free (release->uri);
  g_free (release->name);
  g_free (release->cover_id);
  g_clear_pointer (&release->track_uris, g_ptr_array_unref);
  g_free (release);
}

/* A 16-byte gid at `field` of `msg`, as spotify:<kind>:<base62>. */
static gchar *
dup_uri_from_gid (const guint8 *msg, gsize len, guint32 field, const gchar *kind)
{
  const guint8 *gid = NULL;
  gsize         gid_len = 0;
  if (!pb_find_bytes_field (msg, len, field, &gid, &gid_len) || gid_len != 16)
    return NULL;
  g_autofree gchar *b62 = spotifygtk_gid_to_base62 (gid, gid_len);
  return b62 ? g_strconcat ("spotify:", kind, ":", b62, NULL) : NULL;
}

static void
for_each_entity (const guint8 *data, gsize len, EntityFunc fn, gpointer user_data)
{
  gsize         pos = 0;
  guint32       fnum;
  PbWireType    wtype;
  const guint8 *fdata;
  gsize         flen;
  guint64       fvarint;

  while (pb_read_field (data, len, &pos, &fnum, &wtype, &fdata, &flen, &fvarint)) {
    if (fnum != BER_EXTENDED_METADATA || wtype != PB_WIRE_LENGTH_DELIMITED)
      continue;

    gsize         ipos = 0;
    guint32       ifnum;
    PbWireType    iwtype;
    const guint8 *idata;
    gsize         ilen;
    guint64       ivarint;

    while (pb_read_field (fdata, flen, &ipos, &ifnum, &iwtype, &idata, &ilen, &ivarint)) {
      if (ifnum != EEDA_EXTENSION_DATA || iwtype != PB_WIRE_LENGTH_DELIMITED)
        continue;
      const guint8 *any = NULL; gsize any_len = 0;
      if (!pb_find_bytes_field (idata, ilen, EED_EXTENSION_DATA, &any, &any_len))
        continue;
      const guint8 *entity = NULL; gsize entity_len = 0;
      if (!pb_find_bytes_field (any, any_len, ANY_VALUE, &entity, &entity_len))
        continue;
      fn (entity, entity_len, user_data);
    }
  }
}

static void
collect_group (const guint8 *artist, gsize len, guint32 field,
               SpotifyReleaseGroup group, GPtrArray *out)
{
  gsize         pos = 0;
  guint32       fnum;
  PbWireType    wtype;
  const guint8 *fdata;
  gsize         flen;
  guint64       fvarint;

  while (pb_read_field (artist, len, &pos, &fnum, &wtype, &fdata, &flen, &fvarint)) {
    if (fnum != field || wtype != PB_WIRE_LENGTH_DELIMITED)
      continue;

    /* An AlbumGroup is the same release in several editions; the first is the
     * one to show, or a page lists the same record three times. */
    const guint8 *album = NULL; gsize album_len = 0;
    if (!pb_find_bytes_field (fdata, flen, ALBUM_GROUP_ALBUM, &album, &album_len))
      continue;

    gchar *uri = dup_uri_from_gid (album, album_len, ALBUM_FIELD_GID, "album");
    if (!uri)
      continue;

    SpotifyRelease *r = g_new0 (SpotifyRelease, 1);
    r->uri   = uri;
    r->group = group;
    g_ptr_array_add (out, r);
  }
}

static void
on_artist_releases_entity (const guint8 *artist, gsize len, gpointer user_data)
{
  GPtrArray *out = user_data;
  collect_group (artist, len, ARTIST_FIELD_ALBUM_GROUP,  SPOTIFY_RELEASE_ALBUM,       out);
  collect_group (artist, len, ARTIST_FIELD_SINGLE_GROUP, SPOTIFY_RELEASE_SINGLE,      out);
  collect_group (artist, len, ARTIST_FIELD_COMPIL_GROUP, SPOTIFY_RELEASE_COMPILATION, out);
}

static void
on_album_entity (const guint8 *album, gsize len, gpointer user_data)
{
  GPtrArray *out = user_data;

  gchar *uri = dup_uri_from_gid (album, len, ALBUM_FIELD_GID, "album");
  if (!uri)
    return;

  SpotifyRelease *r = g_new0 (SpotifyRelease, 1);
  r->uri = uri;

  const guint8 *fdata = NULL; gsize flen = 0;
  if (pb_find_bytes_field (album, len, ALBUM_FIELD_NAME, &fdata, &flen))
    r->name = g_strndup ((const gchar *) fdata, flen);

  guint64 v = 0;
  if (pb_find_varint_field (album, len, ALBUM_FIELD_TYPE, &v))
    r->type = (SpotifyAlbumType) v;

  /* Takes the Album, not the date submessage, and zigzag-decodes it -- see
   * the header. Reading Date.year as a plain varint returns 4046 for 2023. */
  r->year = spotifygtk_album_release_year (album, len);

  if (pb_find_bytes_field (album, len, ALBUM_FIELD_COVER_GROUP, &fdata, &flen))
    r->cover_id = dup_widest_image (fdata, flen);

  /* disc is repeated, and so is track within it; both orders matter, so this
   * walks rather than taking the first of each. */
  r->track_uris = g_ptr_array_new_with_free_func (g_free);

  gsize         pos = 0;
  guint32       fnum;
  PbWireType    wtype;
  const guint8 *ddata;
  gsize         dlen;
  guint64       dvarint;

  while (pb_read_field (album, len, &pos, &fnum, &wtype, &ddata, &dlen, &dvarint)) {
    if (fnum != ALBUM_FIELD_DISC || wtype != PB_WIRE_LENGTH_DELIMITED)
      continue;

    gsize         tpos = 0;
    guint32       tfnum;
    PbWireType    twtype;
    const guint8 *tdata;
    gsize         tlen;
    guint64       tvarint;

    while (pb_read_field (ddata, dlen, &tpos, &tfnum, &twtype, &tdata, &tlen, &tvarint)) {
      if (tfnum != DISC_FIELD_TRACK || twtype != PB_WIRE_LENGTH_DELIMITED)
        continue;
      gchar *turi = dup_uri_from_gid (tdata, tlen, TRACK_FIELD_GID, "track");
      if (turi)
        g_ptr_array_add (r->track_uris, turi);
    }
  }

  g_ptr_array_add (out, r);
}

static void
on_releases_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ReleasesClosure *cl = user_data;
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

  g_autoptr(GPtrArray) out =
    g_ptr_array_new_with_free_func ((GDestroyNotify) spotifygtk_release_free);
  for_each_entity (data, len, cl->entity_fn, out);

  if (cl->callback) cl->callback (out, NULL, cl->user_data);
  g_free (cl);
}

/* Both discography calls are the same request with a different kind and a
 * different reader, so they share one sender. */
static void
send_entity_batch (SpotifySpclient *self, const gchar *const *uris, guint n_uris,
                   guint kind, EntityFunc entity_fn,
                   const gchar *bearer_token, const gchar *client_token,
                   SpclientReleasesCallback callback, gpointer user_data)
{
  GByteArray *req_buf = g_byte_array_new ();

  for (guint i = 0; i < n_uris; i++) {
    if (!uris[i] || !*uris[i])
      continue;
    GByteArray *query_buf = g_byte_array_new ();
    pb_write_varint_field (query_buf, 1, kind);

    GByteArray *entity_req_buf = g_byte_array_new ();
    pb_write_bytes_field (entity_req_buf, 1, (const guint8 *) uris[i], strlen (uris[i]));
    pb_write_message_field (entity_req_buf, 2, query_buf->data, query_buf->len);
    pb_write_message_field (req_buf, 2, entity_req_buf->data, entity_req_buf->len);

    g_byte_array_free (query_buf, TRUE);
    g_byte_array_free (entity_req_buf, TRUE);
  }

  g_autofree gchar *url = g_strdup_printf (
    "https://%s/extended-metadata/v0/extended-metadata", SPCLIENT_FALLBACK_HOST);

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

  ReleasesClosure *cl = g_new0 (ReleasesClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;
  cl->entity_fn = entity_fn;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    self->cancellable, on_releases_response, cl);
  g_object_unref (msg);
}

void
spotifygtk_spclient_get_artist_releases (SpotifySpclient         *self,
                                         const gchar             *artist_uri,
                                         const gchar             *bearer_token,
                                         const gchar             *client_token,
                                         SpclientReleasesCallback callback,
                                         gpointer                 user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_SPCLIENT (self));

  if (!artist_uri || !*artist_uri) {
    if (callback) callback (NULL, NULL, user_data);
    return;
  }

  send_entity_batch (self, &artist_uri, 1, EXTENSION_KIND_ARTIST_V4,
                     on_artist_releases_entity, bearer_token, client_token,
                     callback, user_data);
}

void
spotifygtk_spclient_get_albums_metadata (SpotifySpclient         *self,
                                         const gchar *const      *album_uris,
                                         guint                    n_uris,
                                         const gchar             *bearer_token,
                                         const gchar             *client_token,
                                         SpclientReleasesCallback callback,
                                         gpointer                 user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_SPCLIENT (self));

  if (!album_uris || n_uris == 0) {
    if (callback) callback (NULL, NULL, user_data);
    return;
  }

  send_entity_batch (self, album_uris, n_uris, EXTENSION_KIND_ALBUM_V4,
                     on_album_entity, bearer_token, client_token,
                     callback, user_data);
}


/*
 * Artist banner, over pathfinder.
 *
 * Two things had to be right that were not, and each hid the other.
 *
 * 1. THE USER-AGENT. api-partner answers 403 "Client/request not allowed" to
 *    anything that does not look like a browser. Not the client id -- the
 *    shipped client uses keymaster, the same one we do, confirmed in its own
 *    login bundle. Not the client token, the app version, the origin, or the
 *    method: every one of those was varied against a live server and every one
 *    still 403'd. Sending a Chrome UA is the entire difference, and the
 *    request then succeeds with the credentials we already had.
 *
 * 2. THE FIELD IS NOT UNDER `visuals`. It is `artistUnion.headerImage`, at the
 *    top level, an ImageV2 whose `data.sources[]` carry `maxWidth`/`maxHeight`.
 *    `visuals` holds `avatarImage` (the round profile picture) and `gallery`
 *    (promo photos) -- neither is the banner, and reading either one is how
 *    the page spent so long showing a portrait stretched across a landscape
 *    panel.
 *
 * Sources are ~2660x1140. The URLs point at image-cdn-ak.spotifycdn.com, but
 * i.scdn.co serves the same ids, so the id alone is enough and the cover
 * loader needs no new host.
 *
 * Operation name and persisted-query hash come from the shipped bundle
 * (322.js: `new c.l("queryArtistOverview","query","1ac33dda…737c72",null)`).
 * Persisted means the document is never sent; ad-hoc GraphQL is refused with
 * "Missing extensions in the request", so this cannot be narrowed to just the
 * one field we want. See research/artist-images.md.
 */
#define ARTIST_OVERVIEW_HASH \
  "1ac33ddab5d39a3a9c27802774e6d78b9405cc188c6f75aed007df2a32737c72"

/* The trailing path segment of a Spotify image URL is the id. */
static gchar *
dup_image_id_from_url (const gchar *url)
{
  if (!url || !*url)
    return NULL;
  const gchar *slash = strrchr (url, '/');
  const gchar *id = slash ? slash + 1 : url;
  if (!*id)
    return NULL;
  const gchar *q = strchr (id, '?');
  return q ? g_strndup (id, (gsize) (q - id)) : g_strdup (id);
}

static void
on_artist_header_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ArtistClosure *cl = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  if (!body) {
    if (cl->callback) cl->callback (NULL, NULL, error, cl->user_data);
    g_free (cl);
    return;
  }

  gsize len = 0;
  const gchar *json = g_bytes_get_data (body, &len);
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autofree gchar *cover = NULL;

  if (json_parser_load_from_data (parser, json, (gssize) len, NULL)) {
    JsonNode *root = json_parser_get_root (parser);
    JsonObject *o = (root && JSON_NODE_HOLDS_OBJECT (root))
      ? json_node_get_object (root) : NULL;

    /* data.artistUnion.headerImage.data.sources[] */
    const gchar *path[] = { "data", "artistUnion", "headerImage", "data" };
    for (guint i = 0; o && i < G_N_ELEMENTS (path); i++) {
      JsonNode *n = json_object_has_member (o, path[i])
        ? json_object_get_member (o, path[i]) : NULL;
      o = (n && JSON_NODE_HOLDS_OBJECT (n)) ? json_node_get_object (n) : NULL;
    }

    JsonArray *sources = (o && json_object_has_member (o, "sources"))
      ? json_object_get_array_member (o, "sources") : NULL;

    /* Widest wins: the panel is the full width of the page. */
    gint best = -1;
    for (guint i = 0; sources && i < json_array_get_length (sources); i++) {
      JsonObject *src = json_array_get_object_element (sources, i);
      if (!src || !json_object_has_member (src, "url"))
        continue;
      gint w = json_object_has_member (src, "maxWidth")
        ? (gint) json_object_get_int_member (src, "maxWidth") : 0;
      if (w <= best)
        continue;
      g_autofree gchar *id =
        dup_image_id_from_url (json_object_get_string_member (src, "url"));
      if (id) {
        g_free (cover);
        cover = g_steal_pointer (&id);
        best = w;
      }
    }
  }

  if (cl->callback) cl->callback (NULL, cover, NULL, cl->user_data);
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
    if (callback) callback (NULL, NULL, NULL, user_data);
    return;
  }

  g_autofree gchar *variables = g_strdup_printf (
    "{\"uri\":\"%s\",\"locale\":\"\",\"includePrerelease\":true}", artist_uri);
  g_autofree gchar *extensions = g_strdup_printf (
    "{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"%s\"}}",
    ARTIST_OVERVIEW_HASH);

  g_autofree gchar *enc_vars = g_uri_escape_string (variables, NULL, FALSE);
  g_autofree gchar *enc_ext  = g_uri_escape_string (extensions, NULL, FALSE);
  g_autofree gchar *url = g_strdup_printf (
    "%s?operationName=queryArtistOverview&variables=%s&extensions=%s",
    PATHFINDER_URL, enc_vars, enc_ext);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  g_autofree gchar *auth_hdr =
    g_strdup_printf ("Bearer %s", bearer_token ? bearer_token : "");
  soup_message_headers_replace (headers, "Authorization", auth_hdr);
  soup_message_headers_replace (headers, "Accept", "application/json");
  soup_message_headers_replace (headers, "User-Agent", PATHFINDER_UA);
  if (client_token && *client_token)
    soup_message_headers_replace (headers, "Client-Token", client_token);

  ArtistClosure *cl = g_new0 (ArtistClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;

  if (!self->gql_session)
    self->gql_session = soup_session_new_with_options ("user-agent", PATHFINDER_UA, NULL);

  soup_session_send_and_read_async (self->gql_session, msg, G_PRIORITY_DEFAULT,
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
    if (callback) callback (NULL, NULL, NULL, user_data);
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

  if (g_str_has_prefix (context_uri, "spotify:search:"))
    request_expanded_search (self, context_uri, bearer_token, client_token,
                             callback, user_data);
  else
    request_context_resolve (self, context_uri, bearer_token, client_token,
                             callback, user_data);
}

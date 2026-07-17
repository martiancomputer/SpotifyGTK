/*
 * spclient.c — HTTPS client for track metadata / CDN URL resolution.
 * See spclient.h for scope and the apresolve simplification note.
 */

#include "config.h"
#include "spclient.h"
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
  self->session = soup_session_new_with_options ("user-agent", "spotify-native/" APP_VERSION, NULL);
}

SpotifySpclient *
spotifygtk_spclient_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_SPCLIENT, NULL);
}

void spotifygtk_spclient_set_cancellable (SpotifySpclient *self, GCancellable *cancellable)
{ g_set_object (&self->cancellable, cancellable); }

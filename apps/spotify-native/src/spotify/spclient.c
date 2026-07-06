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
};

G_DEFINE_FINAL_TYPE (SpotifySpclient, spotifygtk_spclient, G_TYPE_OBJECT)

typedef struct {
  SpclientStorageCallback callback;
  gpointer                user_data;
} RequestClosure;

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

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, NULL,
                                    on_response, cl);
  g_object_unref (msg);
}

static void
spotifygtk_spclient_dispose (GObject *object)
{
  SpotifySpclient *self = SPOTIFYGTK_SPCLIENT (object);
  g_clear_object (&self->session);
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

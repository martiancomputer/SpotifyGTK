/*
 * collection.c — Collection (Liked Songs) writes over Mercury.
 *
 * See collection.h for the message layout and for why the endpoint is a
 * parameter rather than a constant.
 */

#include "collection.h"
#include "protobuf_min.h"

#include <string.h>

GByteArray *
spotifygtk_collection_build_write (const gchar        *username,
                                   const gchar        *set,
                                   const gchar *const *uris,
                                   guint               n_uris,
                                   gint32              added_at,
                                   gboolean            is_removed)
{
  g_return_val_if_fail (username != NULL && set != NULL, NULL);
  g_return_val_if_fail (uris != NULL || n_uris == 0, NULL);

  GByteArray *out = g_byte_array_new ();

  /* WriteRequest.username (1), .set (2) */
  pb_write_bytes_field (out, 1, (const guint8 *) username, strlen (username));
  pb_write_bytes_field (out, 2, (const guint8 *) set, strlen (set));

  /* WriteRequest.items (3) — repeated, so the tag simply recurs per item. */
  for (guint i = 0; i < n_uris; i++) {
    if (!uris[i] || !*uris[i])
      continue;

    g_autoptr(GByteArray) item = g_byte_array_new ();
    pb_write_bytes_field (item, 1, (const guint8 *) uris[i], strlen (uris[i]));

    /* proto3 does not serialise default-valued scalars, and a real client's
     * bytes reflect that: a zero added_at and a false is_removed are absent,
     * not written as explicit zeroes. Emitting them anyway would still parse,
     * but would not be byte-identical to what Spotify's own client sends,
     * which is the only reference available for a message this thinly
     * documented. */
    if (added_at != 0)
      pb_write_varint_field (item, 2, (guint64) added_at);
    if (is_removed)
      pb_write_varint_field (item, 3, 1);

    pb_write_message_field (out, 3, item->data, item->len);
  }

  return out;
}

typedef struct {
  SpotifyCollectionCallback callback;
  gpointer                  user_data;
} WriteCtx;

static void
on_write_response (MercuryResponse *response, gpointer user_data)
{
  WriteCtx *ctx = user_data;

  /* A missing response is a transport failure, not a rejection; report it as
   * status 0 so a caller can tell the two apart. */
  guint16  status = response ? response->status_code : 0;
  gboolean ok     = (status >= 200 && status < 300);

  if (ctx->callback)
    ctx->callback (ok, status, ctx->user_data);

  g_free (ctx);
}

void
spotifygtk_collection_write (SpotifyMercury            *mercury,
                             const gchar               *endpoint,
                             const gchar               *username,
                             const gchar               *set,
                             const gchar *const        *uris,
                             guint                      n_uris,
                             gboolean                   is_removed,
                             SpotifyCollectionCallback  callback,
                             gpointer                   user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_MERCURY (mercury));
  g_return_if_fail (endpoint != NULL && username != NULL);

  /* added_at is seconds since the epoch, and only meaningful when adding —
   * a removal carries no timestamp. */
  gint32 added_at = is_removed ? 0 : (gint32) (g_get_real_time () / G_USEC_PER_SEC);

  g_autoptr(GByteArray) body =
    spotifygtk_collection_build_write (username, set ? set : SPOTIFYGTK_COLLECTION_SET_LIKED,
                                       uris, n_uris, added_at, is_removed);
  if (!body)
    return;

  g_autoptr(GBytes) payload = g_bytes_new (body->data, body->len);

  WriteCtx *ctx = g_new0 (WriteCtx, 1);
  ctx->callback  = callback;
  ctx->user_data = user_data;

  g_message ("collection: %s %u item(s) via %s",
             is_removed ? "removing" : "adding", n_uris, endpoint);

  spotifygtk_mercury_request (mercury, MERCURY_METHOD_SEND, endpoint,
                              payload, on_write_response, ctx);
}

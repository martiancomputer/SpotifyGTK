/*
 * collection.c — Collection (Liked Songs) writes over Mercury.
 *
 * See collection.h for the message layout and for why the endpoint is a
 * parameter rather than a constant.
 */

#include "collection.h"
#include "track_meta.h"
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

/* ── Reading a page of the collection ─────────────────────────────────────── */

void
spotifygtk_collection_items_free (SpotifyCollectionItem *items, guint n_items)
{
  if (!items)
    return;
  for (guint i = 0; i < n_items; i++)
    g_free (items[i].uri);
  g_free (items);
}

GByteArray *
spotifygtk_collection_build_page_request (const gchar *username,
                                          const gchar *set,
                                          const gchar *pagination_token,
                                          gint32       limit)
{
  g_return_val_if_fail (username != NULL, NULL);

  GByteArray *out = g_byte_array_new ();

  pb_write_bytes_field (out, 1, (const guint8 *) username, strlen (username));
  if (set && *set)
    pb_write_bytes_field (out, 2, (const guint8 *) set, strlen (set));

  /* proto3 omits default-valued fields, so an absent token and a zero limit are
   * left out rather than written as empty/zero -- matching what a real client
   * puts on the wire, which is the whole point of pinning this in a test. */
  if (pagination_token && *pagination_token)
    pb_write_bytes_field (out, 3, (const guint8 *) pagination_token,
                          strlen (pagination_token));
  if (limit > 0)
    pb_write_varint_field (out, 4, (guint64) limit);

  return out;
}

gboolean
spotifygtk_collection_parse_page_response (const guint8           *data,
                                           gsize                   len,
                                           SpotifyCollectionItem **items,
                                           guint                  *n_items,
                                           gchar                 **next_page_token)
{
  g_return_val_if_fail (items != NULL && n_items != NULL, FALSE);

  *items           = NULL;
  *n_items         = 0;
  if (next_page_token)
    *next_page_token = NULL;

  if (!data || len == 0)
    return FALSE;

  GArray *out = g_array_new (FALSE, TRUE, sizeof (SpotifyCollectionItem));

  gsize         pos = 0;
  guint32       field_num;
  PbWireType    wire_type;
  const guint8 *field_data;
  gsize         field_len;
  guint64       field_varint;

  while (pb_read_field (data, len, &pos, &field_num, &wire_type,
                        &field_data, &field_len, &field_varint)) {
    if (field_num == 1 && wire_type == PB_WIRE_LENGTH_DELIMITED) {
      /* repeated CollectionItem items = 1 */
      /*
       * The item on the wire is NOT collection2v2's CollectionItem. That
       * schema says uri = 1 (string), added_at = 2, is_removed = 3, and
       * parsing a live 134KB response against it yields zero items, silently.
       * What the server actually sends is:
       *
       *     type     = 1  varint   0 = catalogue track, 2 = local file
       *     id       = 2  bytes    raw 16-byte GID, or for a local file the
       *                            "artist:album:title:duration" key
       *     added_at = 5  varint   seconds since epoch
       *
       * Confirmed against a real account: 4806 items, GIDs base62-encoding to
       * track ids that resolve. collection2v2.proto is a captured schema that
       * this endpoint does not speak.
       */
      SpotifyCollectionItem item = { 0 };

      guint64 type = 0;
      pb_find_varint_field (field_data, field_len, 1, &type);

      const guint8 *id_data = NULL;
      gsize         id_len  = 0;
      if (pb_find_bytes_field (field_data, field_len, 2, &id_data, &id_len)) {
        if (type == 0 && id_len == 16) {
          g_autofree gchar *b62 = spotifygtk_gid_to_base62 (id_data, id_len);
          if (b62)
            item.uri = g_strconcat ("spotify:track:", b62, NULL);
        } else {
          /* Local files have no catalogue id; keep the key so the count is
           * honest rather than dropping them and under-reporting the set. */
          g_autofree gchar *key = g_strndup ((const gchar *) id_data, id_len);
          item.uri = g_strconcat ("spotify:local:", key, NULL);
        }
      }

      guint64 raw = 0;
      /* added_at is int32, not sint32 -- plain varint, no zigzag. Getting that
       * backwards would halve every timestamp, which is the same class of
       * mistake that once doubled every track duration. */
      if (pb_find_varint_field (field_data, field_len, 5, &raw))
        item.added_at = (gint32) raw;

      if (item.uri)
        g_array_append_val (out, item);
      else
        g_free (item.uri);
    } else if (field_num == 2 && wire_type == PB_WIRE_LENGTH_DELIMITED) {
      if (next_page_token && field_len > 0)
        *next_page_token = g_strndup ((const gchar *) field_data, field_len);
    }
    /* field 3 is sync_token, only needed for the delta path. */
  }

  *n_items = out->len;
  *items   = (SpotifyCollectionItem *) g_array_free (out, FALSE);
  return TRUE;
}

typedef struct {
  SpotifyCollectionPageCallback callback;
  gpointer                      user_data;
} PageCtx;

static void
on_page_response (MercuryResponse *response, gpointer user_data)
{
  PageCtx *ctx = user_data;

  guint16  status = response ? response->status_code : 0;
  gboolean ok     = (status >= 200 && status < 300);

  SpotifyCollectionItem *items = NULL;
  guint                  n     = 0;
  g_autofree gchar      *token = NULL;

  /* A Mercury reply is a list of parts; the protobuf body is the first one
   * after the header part the transport already consumed. */
  GBytes *body = (response && response->parts && response->parts->len > 0)
    ? g_ptr_array_index (response->parts, 0) : NULL;

  if (ok && body) {
    gsize        plen = 0;
    const guint8 *pd  = g_bytes_get_data (body, &plen);
    if (!spotifygtk_collection_parse_page_response (pd, plen, &items, &n, &token)) {
      /* A 2xx whose body will not parse is not a success: report it as such
       * rather than as an empty collection, which would look like "you have
       * liked nothing" instead of "this endpoint answers something else". */
      g_warning ("collection: page response was %u but the body did not parse "
                 "as a PageResponse (%" G_GSIZE_FORMAT " bytes)", status, plen);
      ok = FALSE;
    }
  }

  if (ctx->callback)
    ctx->callback (ok, status, items, n, token, ctx->user_data);

  spotifygtk_collection_items_free (items, n);
  g_free (ctx);
}

void
spotifygtk_collection_read_page (SpotifyMercury                *mercury,
                                 const gchar                   *endpoint,
                                 const gchar                   *username,
                                 const gchar                   *set,
                                 const gchar                   *pagination_token,
                                 gint32                         limit,
                                 SpotifyCollectionPageCallback  callback,
                                 gpointer                       user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_MERCURY (mercury));
  g_return_if_fail (endpoint != NULL && username != NULL);

  g_autoptr(GByteArray) body =
    spotifygtk_collection_build_page_request (username,
                                              set ? set : SPOTIFYGTK_COLLECTION_SET_LIKED,
                                              pagination_token, limit);
  if (!body)
    return;

  g_autoptr(GBytes) payload = g_bytes_new (body->data, body->len);

  PageCtx *ctx = g_new0 (PageCtx, 1);
  ctx->callback  = callback;
  ctx->user_data = user_data;

  spotifygtk_mercury_request (mercury, MERCURY_METHOD_GET, endpoint,
                              payload, on_page_response, ctx);
}

/* ── collection v2 ───────────────────────────────────────────────────────── */

#define V2_URI_WRITE  "hm://collection/v2/write"
#define V2_URI_PAGING "hm://collection/v2/paging"

/* CollectionItem, field numbers from the official client's embedded
 * descriptors: uri = 1, added_at = 2, is_removed = 3. */
static GByteArray *
v2_build_write (const gchar *username, const gchar *set,
                const gchar *const *uris, guint n_uris, gboolean is_removed)
{
  GByteArray *body = g_byte_array_new ();
  pb_write_bytes_field (body, 1, (const guint8 *) username, strlen (username));
  pb_write_bytes_field (body, 2, (const guint8 *) set, strlen (set));

  gint64 now = (gint64) time (NULL);
  for (guint i = 0; i < n_uris; i++) {
    if (!uris[i] || !*uris[i])
      continue;
    g_autoptr(GByteArray) item = g_byte_array_new ();
    pb_write_bytes_field (item, 1, (const guint8 *) uris[i], strlen (uris[i]));
    pb_write_varint_field (item, 2, (guint64) now);
    if (is_removed)
      pb_write_varint_field (item, 3, 1);
    pb_write_message_field (body, 3, item->data, item->len);
  }

  /* client_update_id. The server tolerates a random one; the official client
   * carries the same value in a Collection-Update-Id header. */
  g_autofree gchar *update_id = g_uuid_string_random ();
  pb_write_bytes_field (body, 4, (const guint8 *) update_id, strlen (update_id));
  return body;
}

static void
on_v2_write_response (MercuryResponse *response, gpointer user_data)
{
  WriteCtx *ctx = user_data;
  gboolean ok = response && response->status_code >= 200 && response->status_code < 300;
  if (ctx->callback)
    ctx->callback (ok, response ? (guint16) response->status_code : 0, ctx->user_data);
  g_free (ctx);
}

void
spotifygtk_collection_v2_write (SpotifyMercury *mercury, const gchar *username,
                                const gchar *set, const gchar *const *uris,
                                guint n_uris, gboolean is_removed,
                                SpotifyCollectionCallback callback, gpointer user_data)
{
  g_return_if_fail (mercury != NULL && username != NULL);

  g_autoptr(GByteArray) body =
    v2_build_write (username, set ? set : SPOTIFYGTK_COLLECTION_SET_LIKED,
                    uris, n_uris, is_removed);
  g_autoptr(GBytes) payload = g_bytes_new (body->data, body->len);

  WriteCtx *ctx = g_new0 (WriteCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;

  spotifygtk_mercury_set_content_type (mercury, SPOTIFYGTK_COLLECTION_V2_CONTENT_TYPE);
  spotifygtk_mercury_request_full (mercury, MERCURY_METHOD_SEND, "POST",
                                   V2_URI_WRITE, payload,
                                   on_v2_write_response, ctx);
}

/* PageResponse: items = 1 (CollectionItem), next_page_token = 2. */
static void
on_v2_page_response (MercuryResponse *response, gpointer user_data)
{
  PageCtx *ctx = user_data;
  gboolean ok = response && response->status_code >= 200 && response->status_code < 300;
  GArray *out = g_array_new (FALSE, TRUE, sizeof (SpotifyCollectionItem));
  g_autofree gchar *token = NULL;

  if (ok && response->parts && response->parts->len > 0) {
    gsize len = 0;
    const guint8 *d = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &len);
    gsize pos = 0;
    guint32 fn; PbWireType wt; const guint8 *fd; gsize fl; guint64 fv;
    while (pb_read_field (d, len, &pos, &fn, &wt, &fd, &fl, &fv)) {
      if (fn == 1 && wt == PB_WIRE_LENGTH_DELIMITED) {
        SpotifyCollectionItem item = { 0 };
        const guint8 *u = NULL; gsize ul = 0; guint64 raw = 0;
        if (pb_find_bytes_field (fd, fl, 1, &u, &ul))
          item.uri = g_strndup ((const gchar *) u, ul);
        if (pb_find_varint_field (fd, fl, 2, &raw))
          item.added_at = (gint32) raw;
        if (pb_find_varint_field (fd, fl, 3, &raw))
          item.is_removed = (raw != 0);
        if (item.uri)
          g_array_append_val (out, item);
        else
          g_free (item.uri);
      } else if (fn == 2 && wt == PB_WIRE_LENGTH_DELIMITED) {
        token = g_strndup ((const gchar *) fd, fl);
      }
    }
  }

  if (ctx->callback)
    ctx->callback (ok, response ? (guint16) response->status_code : 0,
                   (SpotifyCollectionItem *) out->data, out->len, token, ctx->user_data);

  for (guint i = 0; i < out->len; i++)
    g_free (g_array_index (out, SpotifyCollectionItem, i).uri);
  g_array_free (out, TRUE);
  g_free (ctx);
}

void
spotifygtk_collection_v2_read_page (SpotifyMercury *mercury, const gchar *username,
                                    const gchar *set, const gchar *pagination_token,
                                    gint32 limit,
                                    SpotifyCollectionPageCallback callback,
                                    gpointer user_data)
{
  g_return_if_fail (mercury != NULL && username != NULL);

  g_autoptr(GByteArray) body =
    spotifygtk_collection_build_page_request (username,
                                              set ? set : SPOTIFYGTK_COLLECTION_SET_LIKED,
                                              pagination_token, limit);
  if (!body)
    return;
  g_autoptr(GBytes) payload = g_bytes_new (body->data, body->len);

  PageCtx *ctx = g_new0 (PageCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;

  spotifygtk_mercury_set_content_type (mercury, SPOTIFYGTK_COLLECTION_V2_CONTENT_TYPE);
  spotifygtk_mercury_request_full (mercury, MERCURY_METHOD_SEND, "POST",
                                   V2_URI_PAGING, payload,
                                   on_v2_page_response, ctx);
}

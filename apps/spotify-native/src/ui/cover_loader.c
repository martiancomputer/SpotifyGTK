/*
 * cover_loader.c — Album artwork fetching, decoding and caching.
 */

#include "cover_loader.h"

#include <libsoup/soup.h>

#define COVER_CDN_BASE "https://i.scdn.co/image/"

/* id (owned) -> GdkTexture (owned). Never evicted: covers are ~60 KiB each
 * and a session touches tens of them, so the ceiling is well under what an
 * LRU's bookkeeping would be worth. If that changes, this is the place. */
static GHashTable *cover_cache = NULL;

/* Requests already in flight, id -> GList of PendingRequest*. Two rows from
 * the same album ask for the same cover at once; without this they would
 * each open a connection for the same bytes. */
static GHashTable *in_flight = NULL;

static SoupSession *cover_session = NULL;

typedef struct {
  SpotifyCoverCallback callback;
  gpointer             user_data;
  GCancellable        *cancellable;   /* owned, may be NULL */
} PendingRequest;

gchar *
spotifygtk_cover_build_url (const gchar *cover_id)
{
  if (!cover_id || !*cover_id)
    return NULL;

  /* Ids are hex from the protobuf, so they cannot contain anything needing
   * escaping — but validate rather than trust, since this becomes a URL. */
  for (const gchar *p = cover_id; *p; p++) {
    if (!g_ascii_isxdigit (*p))
      return NULL;
  }

  return g_strconcat (COVER_CDN_BASE, cover_id, NULL);
}

static void
ensure_initialised (void)
{
  if (cover_cache)
    return;

  cover_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, g_object_unref);
  in_flight   = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  cover_session = soup_session_new ();
}

static void
pending_request_free (PendingRequest *req)
{
  g_clear_object (&req->cancellable);
  g_free (req);
}

/* Hand `texture` (possibly NULL) to everyone waiting on this id. */
static void
complete_waiters (const gchar *cover_id, GdkTexture *texture)
{
  GList *waiters = g_hash_table_lookup (in_flight, cover_id);
  g_hash_table_remove (in_flight, cover_id);

  for (GList *l = waiters; l; l = l->next) {
    PendingRequest *req = l->data;

    /* A cancelled caller is usually a recycled list row. Delivering anyway
     * would paint the wrong album onto it. */
    if (!req->cancellable || !g_cancellable_is_cancelled (req->cancellable))
      req->callback (texture, req->user_data);

    pending_request_free (req);
  }

  g_list_free (waiters);
}

static void
on_cover_fetched (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autofree gchar *cover_id = user_data;
  g_autoptr(GError) error = NULL;

  g_autoptr(GBytes) bytes =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  if (!bytes) {
    g_debug ("cover %s: fetch failed: %s", cover_id,
             error ? error->message : "unknown");
    complete_waiters (cover_id, NULL);
    return;
  }

  /* GTK decodes JPEG/PNG straight into a texture, so no separate image
   * library is needed on this path. */
  g_autoptr(GdkTexture) texture = gdk_texture_new_from_bytes (bytes, &error);
  if (!texture) {
    g_debug ("cover %s: decode failed: %s", cover_id,
             error ? error->message : "unknown");
    complete_waiters (cover_id, NULL);
    return;
  }

  g_debug ("cover %s: decoded %dx%d", cover_id,
           gdk_texture_get_width (texture), gdk_texture_get_height (texture));
  g_hash_table_insert (cover_cache, g_strdup (cover_id), g_object_ref (texture));
  complete_waiters (cover_id, texture);
}

void
spotifygtk_cover_load (const gchar          *cover_id,
                       GCancellable         *cancellable,
                       SpotifyCoverCallback  callback,
                       gpointer              user_data)
{
  g_return_if_fail (callback != NULL);

  ensure_initialised ();

  /* No artwork for this album: answer immediately so callers do not have to
   * branch on it themselves. */
  if (!cover_id || !*cover_id) {
    callback (NULL, user_data);
    return;
  }

  GdkTexture *cached = g_hash_table_lookup (cover_cache, cover_id);
  if (cached) {
    callback (cached, user_data);
    return;
  }

  PendingRequest *req = g_new0 (PendingRequest, 1);
  req->callback    = callback;
  req->user_data   = user_data;
  req->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  /* Join an existing fetch for the same id rather than starting a second. */
  GList *waiters = g_hash_table_lookup (in_flight, cover_id);
  if (waiters) {
    g_hash_table_steal (in_flight, cover_id);
    waiters = g_list_prepend (waiters, req);
    g_hash_table_insert (in_flight, g_strdup (cover_id), waiters);
    return;
  }

  g_autofree gchar *url = spotifygtk_cover_build_url (cover_id);
  if (!url) {
    g_warning ("cover: refusing malformed image id");
    callback (NULL, user_data);
    pending_request_free (req);
    return;
  }

  g_hash_table_insert (in_flight, g_strdup (cover_id), g_list_prepend (NULL, req));

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  soup_session_send_and_read_async (cover_session, msg, G_PRIORITY_LOW, NULL,
                                    on_cover_fetched, g_strdup (cover_id));
  g_object_unref (msg);
}

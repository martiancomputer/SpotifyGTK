/*
 * cover_loader.c — Album artwork fetching, decoding and caching.
 */

#include "cover_loader.h"

#include <libsoup/soup.h>

#include "settings.h"

#define COVER_CDN_BASE "https://i.scdn.co/image/"

/* id (owned) -> GdkTexture (owned).
 *
 * The comment that used to sit here said "covers are ~60 KiB each" and left
 * the cache unbounded. That is the *compressed JPEG* size; GdkTexture holds
 * the *decoded* pixels, 640x640x4 = 1.6 MB each. Scrolling a library decoded
 * ~90 of them and the process doubled in RSS (117 -> 280 MB). Now capped and
 * evicted oldest-first.
 *
 * Eviction is safe while a cover is on screen: GtkPicture holds its own ref,
 * so dropping the cache entry only means a later request re-fetches it, not
 * that a visible image blanks. */
/*
 * Bounded by bytes, not by entries.
 *
 * A count was the wrong unit once two callers wanted different sizes. The album
 * grid decodes at 352px (~484 KB a cover) and shows a few dozen; a track list
 * decodes at 96px (~36 KB) and scrolls past thousands. Forty-eight entries is
 * generous for the first and hopeless for the second -- a Liked Songs scroll
 * misses on essentially every row, so nearly every one becomes an HTTP fetch,
 * which is the stutter. It also meant raising the decode size silently raised
 * the memory ceiling, since the bound never counted bytes.
 *
 * A budget adapts on its own: ~1300 small covers or ~100 large ones, and
 * changing a decode size can no longer move the ceiling.
 */
#define COVER_CACHE_MAX_BYTES (48 * 1024 * 1024)

/* Bytes currently held. GdkTexture does not report its footprint, so this is
 * computed from the dimensions at insert -- exact for the RGBA memory textures
 * this loader creates. */
static gsize cover_cache_bytes = 0;

static gsize
texture_bytes (GdkTexture *texture)
{
  if (!texture)
    return 0;
  return (gsize) gdk_texture_get_width (texture) *
         (gsize) gdk_texture_get_height (texture) * 4;
}

static GHashTable *cover_cache = NULL;

/* Insertion order of the ids in cover_cache, oldest at the head. Kept in
 * step with the table so eviction is O(1) at the front. */
static GQueue cover_order = G_QUEUE_INIT;

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

static gboolean cover_deferred = FALSE;

static void
prefetch_discard (GdkTexture *texture, gpointer user_data)
{
  (void) texture; (void) user_data;   /* the cache insert was the point */
}

void
spotifygtk_cover_prefetch (const gchar *cover_id, gint target_px)
{
  if (!cover_id || !*cover_id || cover_deferred)
    return;
  spotifygtk_cover_load (cover_id, target_px, NULL, prefetch_discard, NULL);
}

void
spotifygtk_cover_set_deferred (gboolean deferred)
{
  cover_deferred = deferred;
}

gboolean
spotifygtk_cover_get_deferred (void)
{
  return cover_deferred;
}

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

/* Carried through the fetch: the cache identity (id@target) and the target
 * size the bytes are decoded at. */
typedef struct {
  gchar *cache_key;
  gint   target_px;
} FetchClosure;

/* Runs on a worker thread: decode the JPEG at the target size and build a
 * texture. Doing this here rather than in the soup callback is what keeps a
 * scroll smooth -- decoding on the main thread blocked a frame per cover, so
 * a burst of arriving covers stuttered the UI. Only data work happens here;
 * the cache insert and delivery hop back to the main thread. */
static void
decode_in_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  GBytes *bytes = task_data;
  gint target_px = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "target-px"));
  g_autoptr(GError) error = NULL;

  g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes (bytes);
  g_autoptr(GdkPixbuf) pixbuf =
    gdk_pixbuf_new_from_stream_at_scale (stream, target_px, target_px, TRUE, NULL, &error);
  if (!pixbuf) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  /* GdkMemoryTexture is just data, so it is safe to build off the main
   * thread; the GBytes copies the scaled pixels. */
  gint     w         = gdk_pixbuf_get_width (pixbuf);
  gint     h         = gdk_pixbuf_get_height (pixbuf);
  gint     rowstride = gdk_pixbuf_get_rowstride (pixbuf);
  gboolean has_alpha = gdk_pixbuf_get_has_alpha (pixbuf);

  g_autoptr(GBytes) pixels =
    g_bytes_new (gdk_pixbuf_get_pixels (pixbuf), (gsize) rowstride * h);
  GdkTexture *texture = gdk_memory_texture_new (
    w, h, has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, pixels, rowstride);

  g_task_return_pointer (task, texture, g_object_unref);
  (void) source; (void) cancellable;
}

/* Back on the main thread: cache the texture and hand it to the waiters. */
static void
on_cover_decoded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autofree gchar *cache_key = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkTexture) texture = g_task_propagate_pointer (G_TASK (result), &error);

  if (!texture) {
    g_debug ("cover %s: decode failed: %s", cache_key,
             error ? error->message : "unknown");
    complete_waiters (cache_key, NULL);
    return;
  }

  gsize incoming = texture_bytes (texture);

  while (cover_cache_bytes + incoming > COVER_CACHE_MAX_BYTES &&
         !g_queue_is_empty (&cover_order)) {
    g_autofree gchar *oldest = g_queue_pop_head (&cover_order);
    if (!oldest)
      break;
    GdkTexture *victim = g_hash_table_lookup (cover_cache, oldest);
    if (victim) {
      gsize freed = texture_bytes (victim);
      cover_cache_bytes -= MIN (freed, cover_cache_bytes);
    }
    g_hash_table_remove (cover_cache, oldest);
  }

  g_hash_table_insert (cover_cache, g_strdup (cache_key), g_object_ref (texture));
  g_queue_push_tail (&cover_order, g_strdup (cache_key));
  cover_cache_bytes += incoming;

  complete_waiters (cache_key, texture);
  (void) source;
}

static void
on_cover_fetched (GObject *source, GAsyncResult *result, gpointer user_data)
{
  FetchClosure *fetch = user_data;
  gchar *cache_key = fetch->cache_key;   /* ownership moves to the decode task */
  gint target_px = fetch->target_px;
  g_free (fetch);

  g_autoptr(GError) error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  if (!bytes) {
    g_debug ("cover %s: fetch failed: %s", cache_key,
             error ? error->message : "unknown");
    complete_waiters (cache_key, NULL);
    g_free (cache_key);
    return;
  }

  /* Decode off the main thread. The bytes and target size ride along; the
   * key is the callback's user_data. */
  GTask *task = g_task_new (NULL, NULL, on_cover_decoded, cache_key);
  g_object_set_data (G_OBJECT (task), "target-px", GINT_TO_POINTER (target_px));
  g_task_set_task_data (task, bytes, (GDestroyNotify) g_bytes_unref);
  g_task_run_in_thread (task, decode_in_thread);
  g_object_unref (task);
}

void
spotifygtk_cover_load (const gchar          *cover_id,
                       gint                  target_px,
                       GCancellable         *cancellable,
                       SpotifyCoverCallback  callback,
                       gpointer              user_data)
{
  g_return_if_fail (callback != NULL);

  /* Text-only mode is enforced here rather than at each display site, so a
   * new artwork consumer cannot forget to honour it -- and so the image is
   * never fetched at all, which is the point of the setting. */
  if (spotifygtk_settings_get_media_mode (spotifygtk_settings_get_default ())
      == SPOTIFYGTK_MEDIA_TEXT_ONLY) {
    callback (NULL, user_data);
    return;
  }

  ensure_initialised ();

  /* No artwork for this album: answer immediately so callers do not have to
   * branch on it themselves. */
  if (!cover_id || !*cover_id) {
    callback (NULL, user_data);
    return;
  }

  if (target_px <= 0)
    target_px = 96;

  /* The same album at two different sizes must not share a cache slot, or a
   * row's 96px decode would satisfy the panel's request and show blurry. */
  g_autofree gchar *cache_key = g_strdup_printf ("%s@%d", cover_id, target_px);

  GdkTexture *cached = g_hash_table_lookup (cover_cache, cache_key);
  if (cached) {
    callback (cached, user_data);
    return;
  }

  /* Deferred: a miss costs nothing rather than queueing a fetch for a row the
   * scroll is already past. The caller re-requests on settle. */
  if (cover_deferred) {
    callback (NULL, user_data);
    return;
  }

  PendingRequest *req = g_new0 (PendingRequest, 1);
  req->callback    = callback;
  req->user_data   = user_data;
  req->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  /* Join an existing fetch for the same (id, size) rather than starting a
   * second.
   *
   * g_hash_table_insert with a key that compares equal replaces the value
   * and frees the *passed* key, keeping the original — so the list head can
   * be updated in place. The previous version used g_hash_table_steal here,
   * which removes the entry without freeing its key; that original strdup'd
   * key was then dropped on the floor, one leaked string per coalesced
   * request. */
  GList *waiters = g_hash_table_lookup (in_flight, cache_key);
  if (waiters) {
    waiters = g_list_prepend (waiters, req);
    g_hash_table_insert (in_flight, g_strdup (cache_key), waiters);
    return;
  }

  g_autofree gchar *url = spotifygtk_cover_build_url (cover_id);
  if (!url) {
    g_warning ("cover: refusing malformed image id");
    callback (NULL, user_data);
    pending_request_free (req);
    return;
  }

  g_hash_table_insert (in_flight, g_strdup (cache_key), g_list_prepend (NULL, req));

  FetchClosure *fetch = g_new0 (FetchClosure, 1);
  fetch->cache_key = g_strdup (cache_key);
  fetch->target_px = target_px;

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  soup_session_send_and_read_async (cover_session, msg, G_PRIORITY_LOW, NULL,
                                    on_cover_fetched, fetch);
  g_object_unref (msg);
}

/*
 * cover_loader.c — Album artwork fetching, decoding and caching.
 */

#include "cover_loader.h"
#include <stdlib.h>

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

/* Evict oldest-first until the cache fits in `budget`. Shared by the insert
 * path and by spotifygtk_cover_trim_to(). */
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

static void
cover_evict_to (gsize budget)
{
  while (cover_cache_bytes > budget && !g_queue_is_empty (&cover_order)) {
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
}

/* Requests already in flight, id -> GList of PendingRequest*. Two rows from
 * the same album ask for the same cover at once; without this they would
 * each open a connection for the same bytes. */
static GHashTable *in_flight = NULL;

static SoupSession *cover_session = NULL;

static struct {
  guint  hits;          /* served from cache */
  guint  misses;        /* required a fetch */
  guint  joined;        /* deduplicated onto an in-flight fetch */
  guint  deferred;      /* dropped because a scroll was in progress */
  guint  failures;
  guint  fetched;       /* completed fetches, denominator for the times below */
  gint64 fetch_us;      /* cumulative network time */
  gint64 decode_us;     /* cumulative decode time */
  gint64 bytes;         /* cumulative payload */
  gint64 worst_fetch_us;
  gint64 queue_us;      /* enqueue -> connection assigned */
  gint64 transfer_us;   /* first byte -> last byte */
  guint  metered;       /* fetches with usable metrics */
  guint  dropped;       /* queued, then every waiter went away before its turn */
  guint  peak_queue;
} cover_stats;

/*
 * Outstanding fetches are capped and the backlog is a stack, not a line.
 *
 * Requests used to go straight to libsoup as each row bound, so a scroll past
 * three hundred rows issued three hundred fetches for the fifteen on screen.
 * Virtualisation bounds widgets, not requests: a card is recycled, but the
 * fetch it started outlives the binding and stays in the pipe.
 *
 * Measured before this: 257 fetches averaging 2.7 seconds each, of which only
 * ~210ms was libsoup's own queue and ~48ms was on the wire. The remaining
 * ~2.4s was the backlog we had handed it. The image CDN was never slow.
 *
 * LIFO is the part that matters. The newest request is for a row on screen
 * now; the oldest is for one scrolled past long ago. Served first-in-first-out,
 * every visible cover waits behind a queue of covers nobody will see -- which
 * is exactly the "still loading what I already scrolled past" effect. A stack
 * fixes that ordering without predicting anything.
 */
#define COVER_MAX_INFLIGHT 8

typedef struct {
  gchar *cache_key;
  gint   target_px;
} QueuedFetch;

static GQueue cover_queue = G_QUEUE_INIT;   /* QueuedFetch*, newest at the tail */
static guint  cover_active = 0;

static void issue_fetch (const gchar *cache_key, gint target_px);
static void on_cover_fetched (GObject *source, GAsyncResult *result, gpointer user_data);

static void
queued_fetch_free (QueuedFetch *q)
{
  if (!q)
    return;
  g_free (q->cache_key);
  g_free (q);
}

/* Start as many queued fetches as the cap allows, newest first. */
static void
cover_pump (void)
{
  while (cover_active < COVER_MAX_INFLIGHT) {
    QueuedFetch *q = g_queue_pop_tail (&cover_queue);
    if (!q)
      return;

    /* Every waiter for this cover has gone -- the rows were recycled while it
     * sat in the queue. Dropping it here is the whole point of queueing in our
     * own code rather than libsoup's, where it could not be reconsidered. */
    if (!g_hash_table_contains (in_flight, q->cache_key)) {
      cover_stats.dropped++;
      queued_fetch_free (q);
      continue;
    }

    cover_active++;
    issue_fetch (q->cache_key, q->target_px);
    queued_fetch_free (q);
  }
}

typedef struct {
  SpotifyCoverCallback callback;
  gpointer             user_data;
  GCancellable        *cancellable;   /* owned, may be NULL */
} PendingRequest;

/*
 * Cover-fetch statistics.
 *
 * Kept because the interesting questions about this path -- is the network or
 * the decode the cost, does the cache actually hit, how big are these images --
 * were all unanswerable. The counters are plain integers touched only from the
 * main thread (requests start there and complete there), so no locking.
 *
 * Reported on demand rather than per request: one line per cover would drown
 * the log during a scroll, which is exactly when the numbers matter.
 */

void
spotifygtk_cover_log_stats (const gchar *context)
{
  guint asked = cover_stats.hits + cover_stats.misses +
                cover_stats.joined + cover_stats.deferred;
  if (asked == 0) {
    g_message ("cover stats (%s): nothing requested yet", context ? context : "");
    return;
  }

  g_message ("cover stats (%s): %u asked -- %u cached (%.0f%%), %u fetched, "
             "%u joined, %u deferred, %u failed",
             context ? context : "", asked,
             cover_stats.hits, 100.0 * cover_stats.hits / asked,
             cover_stats.misses, cover_stats.joined, cover_stats.deferred,
             cover_stats.failures);
  g_message ("cover stats (%s): %u dropped before issue, %u queued now, "
             "peak queue %u, %u in flight (cap %d)",
             context ? context : "", cover_stats.dropped,
             g_queue_get_length (&cover_queue), cover_stats.peak_queue,
             cover_active, COVER_MAX_INFLIGHT);

  if (cover_stats.fetched > 0) {
    g_message ("cover stats (%s): fetch avg %.0f ms, worst %.0f ms; "
               "decode avg %.0f ms; avg payload %.1f KB; %u in cache, %.1f MB",
               context ? context : "",
               cover_stats.fetch_us  / 1000.0 / cover_stats.fetched,
               cover_stats.worst_fetch_us / 1000.0,
               cover_stats.decode_us / 1000.0 / cover_stats.fetched,
               cover_stats.bytes / 1024.0 / cover_stats.fetched,
               cover_cache ? g_hash_table_size (cover_cache) : 0,
               cover_cache_bytes / 1048576.0);
  }

  if (cover_stats.metered > 0) {
    g_message ("cover stats (%s): of that, queued avg %.0f ms, on the wire avg %.0f ms "
               "(%u measured)",
               context ? context : "",
               cover_stats.queue_us    / 1000.0 / cover_stats.metered,
               cover_stats.transfer_us / 1000.0 / cover_stats.metered,
               cover_stats.metered);
  }
}

static gboolean cover_deferred = FALSE;

static void cover_load_internal (const gchar *cover_id, gint target_px,
                                 GCancellable *cancellable,
                                 SpotifyCoverCallback callback,
                                 gpointer user_data, gboolean deferrable);

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
  cover_load_internal (cover_id, target_px, NULL, prefetch_discard, NULL, TRUE);
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
  /*
   * Connection concurrency, overridable so it can be measured rather than
   * argued about. libsoup defaults to 6 per host, and every cover comes from
   * one host, so a few hundred requests queue behind those six -- which is why
   * a 103 KB image appeared to take 1.6 seconds.
   */
  const gchar *conns_env = g_getenv ("SPOTIFY_COVER_CONNS");
  guint conns = conns_env ? (guint) MAX (1, atoi (conns_env)) : 6;

  cover_session = soup_session_new_with_options (
    "max-conns-per-host", conns,
    "max-conns", MAX (conns, 24),
    NULL);
  g_message ("cover: session using %u connections per host", conns);
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
  gint64 started_us;    /* request issued; split into network and decode below */
  gint64 fetched_us;    /* bytes in hand, decode about to begin */
  SoupMessage *message; /* owned, for its metrics */
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

  gint start_ms = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (result), "decode-start"));
  if (start_ms > 0)
    cover_stats.decode_us += (g_get_monotonic_time () - (gint64) start_ms * 1000);

  if (!texture) {
    cover_stats.failures++;
    g_debug ("cover %s: decode failed: %s", cache_key,
             error ? error->message : "unknown");
    complete_waiters (cache_key, NULL);
    return;
  }

  gsize incoming = texture_bytes (texture);

  cover_evict_to (COVER_CACHE_MAX_BYTES > incoming
                    ? COVER_CACHE_MAX_BYTES - incoming : 0);

  g_hash_table_insert (cover_cache, g_strdup (cache_key), g_object_ref (texture));
  g_queue_push_tail (&cover_order, g_strdup (cache_key));
  cover_cache_bytes += incoming;

  complete_waiters (cache_key, texture);
  (void) source;
}

static void
issue_fetch (const gchar *cache_key, gint target_px)
{
  g_autofree gchar *cover_id = g_strdup (cache_key);
  gchar *at = strrchr (cover_id, '@');
  if (at)
    *at = '\0';

  g_autofree gchar *url = spotifygtk_cover_build_url (cover_id);
  if (!url) {
    cover_active--;
    complete_waiters (cache_key, NULL);
    return;
  }

  FetchClosure *fetch = g_new0 (FetchClosure, 1);
  fetch->cache_key = g_strdup (cache_key);
  fetch->target_px = target_px;
  fetch->started_us = g_get_monotonic_time ();

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  /* Metrics are what let queue wait be told apart from transfer time. Without
   * them the only measurable span is enqueue-to-completion, which conflates
   * the two. */
  soup_message_add_flags (msg, SOUP_MESSAGE_COLLECT_METRICS);
  fetch->message = g_object_ref (msg);
  soup_session_send_and_read_async (cover_session, msg, G_PRIORITY_LOW, NULL,
                                    on_cover_fetched, fetch);
  g_object_unref (msg);
}

static void
on_cover_fetched (GObject *source, GAsyncResult *result, gpointer user_data)
{
  FetchClosure *fetch = user_data;
  gchar *cache_key = fetch->cache_key;   /* ownership moves to the decode task */
  gint target_px = fetch->target_px;

  /*
   * The closure is freed at the end, not here.
   *
   * It used to be freed on the line after these two locals were taken, and the
   * timing block below then read it anyway -- a use-after-free that went
   * unnoticed because freed memory usually still holds the old values, so the
   * numbers looked plausible. The metrics work made it obvious, because it
   * dereferences a GObject pointer out of the same freed struct.
   */
  g_autoptr(GError) error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  if (fetch->started_us) {
    gint64 took = g_get_monotonic_time () - fetch->started_us;
    if (bytes) {
      cover_stats.fetched++;
      cover_stats.fetch_us += took;
      cover_stats.bytes += (gint64) g_bytes_get_size (bytes);
      if (took > cover_stats.worst_fetch_us)
        cover_stats.worst_fetch_us = took;

      if (fetch->message) {
        SoupMessageMetrics *m = soup_message_get_metrics (fetch->message);
        if (m) {
          guint64 fetch_start   = soup_message_metrics_get_fetch_start (m);
          guint64 request_start = soup_message_metrics_get_request_start (m);
          guint64 resp_start    = soup_message_metrics_get_response_start (m);
          guint64 resp_end      = soup_message_metrics_get_response_end (m);
          if (request_start > fetch_start && resp_end >= resp_start && resp_start > 0) {
            cover_stats.queue_us    += (gint64) (request_start - fetch_start);
            cover_stats.transfer_us += (gint64) (resp_end - resp_start);
            cover_stats.metered++;
          }
        }
      }
    }
  }

  if (!bytes) {
    g_debug ("cover %s: fetch failed: %s", cache_key,
             error ? error->message : "unknown");
    complete_waiters (cache_key, NULL);
    g_free (cache_key);
    cover_stats.failures++;
    g_clear_object (&fetch->message);
    g_free (fetch);
    cover_active--;
    cover_pump ();
    return;
  }

  /* Decode off the main thread. The bytes and target size ride along; the
   * key is the callback's user_data. */
  GTask *task = g_task_new (NULL, NULL, on_cover_decoded, cache_key);
  /* Carried on the task rather than accumulated in decode_in_thread: that runs
   * on a worker and every other counter here is written from the main thread,
   * so timing it from both ends of the hop keeps the whole struct single
   * threaded and needs no atomics. It measures dispatch plus decode, which is
   * the figure that matters anyway -- a decode that waits on a busy pool costs
   * the user the same as a slow one. */
  g_object_set_data (G_OBJECT (task), "decode-start",
                     GINT_TO_POINTER ((gint) (g_get_monotonic_time () / 1000)));
  g_object_set_data (G_OBJECT (task), "target-px", GINT_TO_POINTER (target_px));
  g_task_set_task_data (task, bytes, (GDestroyNotify) g_bytes_unref);
  g_task_run_in_thread (task, decode_in_thread);
  g_object_unref (task);

  g_clear_object (&fetch->message);
  g_free (fetch);

  /* Released once the bytes are in hand rather than after the decode: the
   * decode is off-thread and 20ms, so holding the slot across it would idle
   * the network for no reason. */
  cover_active--;
  cover_pump ();
}

void
spotifygtk_cover_load (const gchar          *cover_id,
                       gint                  target_px,
                       GCancellable         *cancellable,
                       SpotifyCoverCallback  callback,
                       gpointer              user_data)
{
  cover_load_internal (cover_id, target_px, cancellable, callback, user_data, FALSE);
}

void
spotifygtk_cover_load_deferrable (const gchar          *cover_id,
                                  gint                  target_px,
                                  GCancellable         *cancellable,
                                  SpotifyCoverCallback  callback,
                                  gpointer              user_data)
{
  cover_load_internal (cover_id, target_px, cancellable, callback, user_data, TRUE);
}

static void
cover_load_internal (const gchar          *cover_id,
                     gint                  target_px,
                     GCancellable         *cancellable,
                     SpotifyCoverCallback  callback,
                     gpointer              user_data,
                     gboolean              deferrable)
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
    cover_stats.hits++;
    callback (cached, user_data);
    return;
  }

  /* Deferred: a miss costs nothing rather than queueing a fetch for a row the
   * scroll is already past. Only for callers that re-request on settle -- see
   * spotifygtk_cover_load_deferrable(). */
  if (deferrable && cover_deferred) {
    cover_stats.deferred++;
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
    cover_stats.joined++;
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

  cover_stats.misses++;
  g_hash_table_insert (in_flight, g_strdup (cache_key), g_list_prepend (NULL, req));

  QueuedFetch *q = g_new0 (QueuedFetch, 1);
  q->cache_key = g_strdup (cache_key);
  q->target_px = target_px;
  g_queue_push_tail (&cover_queue, q);
  if (g_queue_get_length (&cover_queue) > cover_stats.peak_queue)
    cover_stats.peak_queue = g_queue_get_length (&cover_queue);
  cover_pump ();
}

/*
 * Drop cached art down to `max_bytes`, oldest first.
 *
 * The cache only ever evicted on insert, so once it reached its ceiling it
 * stayed there for the life of the process -- including while the window was
 * minimised, where tens of megabytes of texture were held for nothing anyone
 * could see. Nothing else releases it: these are GdkTextures held by this
 * table alone, so no amount of malloc tuning touches them.
 *
 * Cheap to get wrong in the other direction, though: trim too eagerly and
 * scrolling back up refetches everything. Only worth doing when the art is
 * demonstrably not being looked at.
 */
void
spotifygtk_cover_trim_to (gsize max_bytes)
{
  gsize before = cover_cache_bytes;
  cover_evict_to (max_bytes);
  if (before != cover_cache_bytes)
    g_message ("cover: trimmed cache %.1f MB -> %.1f MB",
               before / 1048576.0, cover_cache_bytes / 1048576.0);
}

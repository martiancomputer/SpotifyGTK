/*
 * session.c — Long-lived authenticated Spotify session.
 *
 * The sign-in chain here is the one proven by the SPOTIFY_PROBE_CONTEXT
 * probe in main.c: AP connect → login → client-token → login5. The
 * difference is that it stops there and stays open, instead of continuing
 * into a playback pipeline and tearing itself down.
 */

#include "config.h"
#include "session.h"

#include "ap.h"
#include "clienttoken.h"
#include "login5.h"
#include "native_auth.h"
#include "spclient.h"

#include <string.h>

struct _SpotifyNativeSession {
  GObject parent_instance;

  /* Worker thread + its private context. Created in start(), torn down in
   * dispose(). Everything protocol-related runs here. */
  GThread      *thread;
  GMainContext *context;
  GMainLoop    *loop;

  /* Context the session was created on — where signals are emitted. */
  GMainContext *caller_context;

  GCancellable *cancellable;

  /* Worker-owned protocol objects. */
  SpotifyApSession   *ap;
  SpotifyClientToken *client_token_client;
  SpotifyLogin5      *login5_client;
  SpotifySpclient    *spclient;
  gchar              *device_id;
  gchar              *login_token;
  guint               connect_attempts;

  /* Worker-thread only: the refresh handshake and the requests waiting on it. */
  gboolean  refreshing;
  GList    *pending_ops;

  /* Shared state: written by the worker, read by the main thread. */
  GMutex   lock;
  SpotifyNativeSessionState state;
  gchar   *bearer_token;
  gchar   *client_token;
  gchar   *username;
  gint64   bearer_expires_at;   /* monotonic; 0 = unknown */
  gboolean stopping;            /* dispose has begun; worker must not start its loop */
};

/* Re-mint this far ahead of expiry, so a request issued just under the wire
 * does not land after it. */
#define BEARER_REFRESH_MARGIN_US (60 * G_USEC_PER_SEC)

G_DEFINE_FINAL_TYPE (SpotifyNativeSession, spotifygtk_native_session, G_TYPE_OBJECT)

enum { STATE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

void
spotifygtk_native_track_free (SpotifyNativeTrack *track)
{
  if (!track)
    return;
  g_free (track->uri);
  g_free (track->name);
  g_free (track->artists);
  g_free (track->album);
  g_free (track->cover_id);
  g_free (track->album_uri);
  g_free (track->artist_uri);
  g_free (track);
}

SpotifyNativeTrack *
spotifygtk_native_track_copy (const SpotifyNativeTrack *track)
{
  if (!track)
    return NULL;

  SpotifyNativeTrack *copy = g_new0 (SpotifyNativeTrack, 1);
  copy->uri         = g_strdup (track->uri);
  copy->name        = g_strdup (track->name);
  copy->artists     = g_strdup (track->artists);
  copy->album       = g_strdup (track->album);
  copy->duration_ms = track->duration_ms;
  copy->is_explicit = track->is_explicit;
  copy->cover_id    = g_strdup (track->cover_id);
  copy->album_uri   = g_strdup (track->album_uri);
  copy->artist_uri  = g_strdup (track->artist_uri);
  return copy;
}

/* ── State reporting ─────────────────────────────────────────────────────── */

typedef struct {
  SpotifyNativeSession     *session;
  SpotifyNativeSessionState state;
  gchar                    *message;
} StateEvent;

static gboolean
dispatch_state (gpointer user_data)
{
  StateEvent *event = user_data;
  g_signal_emit (event->session, signals[STATE_CHANGED], 0,
                 (gint) event->state, event->message);
  g_object_unref (event->session);
  g_free (event->message);
  g_free (event);
  return G_SOURCE_REMOVE;
}

/* Called on the worker thread. Updates shared state under the lock, then
 * marshals the signal to the caller's context. */
static void
set_state (SpotifyNativeSession *self, SpotifyNativeSessionState state,
           const gchar *message)
{
  g_mutex_lock (&self->lock);
  self->state = state;
  g_mutex_unlock (&self->lock);

  StateEvent *event = g_new0 (StateEvent, 1);
  event->session = g_object_ref (self);
  event->state   = state;
  event->message = g_strdup (message);
  g_main_context_invoke (self->caller_context, dispatch_state, event);
}

static void
fail_session (SpotifyNativeSession *self, const gchar *message)
{
  g_warning ("session: %s", message);
  set_state (self, SPOTIFYGTK_SESSION_FAILED, message);
}

/* ── Sign-in chain (worker thread) ───────────────────────────────────────── */

static gchar *
generate_device_id (void)
{
  /* Same shape as connect.c and the harness: 40 hex chars. Per-session is
   * fine; persisting it would make "this device" recognisable across
   * restarts, which matters for Connect but not for catalog reads. */
  GString *hex = g_string_new (NULL);
  GRand   *r   = g_rand_new ();
  for (int i = 0; i < 20; i++)
    g_string_append_printf (hex, "%02x", (guint8) g_rand_int_range (r, 0, 256));
  g_rand_free (r);
  return g_string_free (hex, FALSE);
}

static void flush_pending_ops (SpotifyNativeSession *self, gboolean refresh_ok);

static void
on_login5_result (const gchar *access_token, gint32 expires_in_seconds,
                  GError *error, gpointer user_data)
{
  SpotifyNativeSession *self = user_data;
  gboolean was_refresh = self->refreshing;

  self->refreshing = FALSE;

  if (!access_token) {
    g_autofree gchar *msg = g_strdup_printf ("login5 failed: %s",
      error ? error->message : "unknown error");
    if (was_refresh) {
      /* A failed *refresh* is not the same as a failed sign-in: the AP
       * session is still up, so report it and let the next request retry
       * rather than tearing the whole session down. */
      g_warning ("session: bearer refresh failed: %s", msg);
      flush_pending_ops (self, FALSE);
      return;
    }
    fail_session (self, msg);
    return;
  }

  g_mutex_lock (&self->lock);
  g_free (self->bearer_token);
  self->bearer_token = g_strdup (access_token);
  /* Track expiry so requests can re-mint before the server starts
   * rejecting them. Without this the session keeps reporting READY while
   * every call 401s -- a silent death about an hour in. */
  self->bearer_expires_at = g_get_monotonic_time ()
                          + (gint64) expires_in_seconds * G_USEC_PER_SEC;
  g_mutex_unlock (&self->lock);

  if (was_refresh) {
    g_message ("session: bearer refreshed (expires in %ds)", expires_in_seconds);
    flush_pending_ops (self, TRUE);
    return;
  }

  /* Created on the worker thread so its SoupSession binds to the worker's
   * context, not GTK's. */
  self->spclient = spotifygtk_spclient_new ();
  spotifygtk_spclient_set_cancellable (self->spclient, self->cancellable);

  g_message ("session: ready (bearer expires in %ds)", expires_in_seconds);
  set_state (self, SPOTIFYGTK_SESSION_READY, "Signed in.");
}

/* Re-mint the bearer using the AP session's reusable credentials, which are
 * still held -- this is the same login5 call the initial sign-in makes, not
 * a fresh AP handshake. */
static void
start_bearer_refresh (SpotifyNativeSession *self)
{
  if (self->refreshing)
    return;

  gsize         creds_len  = 0;
  const guint8 *creds      = spotifygtk_ap_session_get_reusable_creds (self->ap, &creds_len);
  guint64       creds_type = spotifygtk_ap_session_get_reusable_creds_type (self->ap);
  const gchar  *username   = spotifygtk_ap_session_get_username (self->ap);

  if (!creds) {
    g_warning ("session: cannot refresh bearer -- no reusable credentials");
    flush_pending_ops (self, FALSE);
    return;
  }

  g_mutex_lock (&self->lock);
  g_autofree gchar *ctoken = g_strdup (self->client_token);
  g_mutex_unlock (&self->lock);

  self->refreshing = TRUE;
  g_message ("session: bearer expired or expiring, re-minting via login5");

  g_clear_object (&self->login5_client);
  self->login5_client = spotifygtk_login5_new ();
  spotifygtk_login5_set_cancellable (self->login5_client, self->cancellable);
  spotifygtk_login5_auth_token (self->login5_client,
                                NATIVE_AUTH_CLIENT_ID, self->device_id,
                                username, creds, creds_len, creds_type,
                                ctoken, on_login5_result, self);
}

static void
on_client_token_result (const gchar *token, gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  if (token) {
    g_mutex_lock (&self->lock);
    g_free (self->client_token);
    self->client_token = g_strdup (token);
    g_mutex_unlock (&self->lock);
  } else {
    /* login5 requires one per spclient.rs; continue so the failure surfaces
     * as a login5 error rather than being masked here. */
    g_warning ("session: no client-token obtained; continuing to login5 anyway");
  }

  const gchar  *username   = spotifygtk_ap_session_get_username (self->ap);
  gsize         creds_len  = 0;
  const guint8 *creds      = spotifygtk_ap_session_get_reusable_creds (self->ap, &creds_len);
  guint64       creds_type = spotifygtk_ap_session_get_reusable_creds_type (self->ap);

  if (!creds) {
    fail_session (self, "no reusable credentials in APWelcome; cannot reach login5");
    return;
  }

  g_mutex_lock (&self->lock);
  g_free (self->username);
  self->username = g_strdup (username);
  g_mutex_unlock (&self->lock);

  self->login5_client = spotifygtk_login5_new ();
  spotifygtk_login5_set_cancellable (self->login5_client, self->cancellable);
  spotifygtk_login5_auth_token (self->login5_client,
                                NATIVE_AUTH_CLIENT_ID, self->device_id,
                                username, creds, creds_len, creds_type,
                                token, on_login5_result, self);
}

static void
on_ap_login_result (gboolean success, const gchar *username,
                    GError *error, gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  if (!success) {
    g_autofree gchar *msg = g_strdup_printf ("AP login failed: %s",
      error ? error->message : "unknown error");
    fail_session (self, msg);
    return;
  }

  g_message ("session: AP login succeeded%s", username ? "" : " (no username returned)");
  set_state (self, SPOTIFYGTK_SESSION_CONNECTING, "Signed in; authorizing streaming session.");

  self->client_token_client = spotifygtk_client_token_new ();
  spotifygtk_client_token_set_cancellable (self->client_token_client, self->cancellable);
  spotifygtk_client_token_request (self->client_token_client,
                                   NATIVE_AUTH_CLIENT_ID, self->device_id,
                                   on_client_token_result, self);
}

/* Spotify refuses new AP connections when a client opens them too quickly.
 * Because the playback engine still logs in per track, a listening session
 * can burn through its allowance and leave even this sign-in refused, so a
 * single "Connection refused" is not a reason to give up. */
#define AP_CONNECT_MAX_ATTEMPTS 4

static void on_ap_connected (GObject *source, GAsyncResult *result, gpointer user_data);

static gboolean
retry_ap_connect (gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  if (g_cancellable_is_cancelled (self->cancellable))
    return G_SOURCE_REMOVE;

  spotifygtk_ap_session_connect (self->ap, NULL, on_ap_connected, self);
  return G_SOURCE_REMOVE;
}

static void
on_ap_connected (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyApSession     *ap   = SPOTIFYGTK_AP_SESSION (source);
  SpotifyNativeSession *self = user_data;
  g_autoptr(GError)     err  = NULL;

  if (!spotifygtk_ap_session_connect_finish (ap, result, &err)) {
    if (self->connect_attempts < AP_CONNECT_MAX_ATTEMPTS &&
        !g_cancellable_is_cancelled (self->cancellable)) {
      self->connect_attempts++;
      guint delay_ms = 500u * self->connect_attempts;
      g_warning ("session: AP connect failed (%s); retry %u/%u in %ums",
                 err ? err->message : "unknown error",
                 self->connect_attempts, AP_CONNECT_MAX_ATTEMPTS, delay_ms);

      /* Attach to the session's own context, not the global default one that
       * g_timeout_add() uses — otherwise the retry runs on the GTK main thread
       * and every request chained off it (client-token, login5, spclient) binds
       * to the main context, which is exactly the isolation this class exists
       * to guarantee. See the matching note in main.c. */
      GSource *retry = g_timeout_source_new (delay_ms);
      g_source_set_callback (retry, retry_ap_connect, self, NULL);
      g_source_attach (retry, self->context);
      g_source_unref (retry);
      return;
    }

    g_autofree gchar *msg = g_strdup_printf ("AP handshake failed: %s",
      err ? err->message : "unknown error");
    fail_session (self, msg);
    return;
  }

  spotifygtk_ap_session_start_receiving (ap);
  set_state (self, SPOTIFYGTK_SESSION_CONNECTING, "Secure channel established; signing in.");

  spotifygtk_ap_session_login (ap, g_getenv ("SPOTIFY_USERNAME"),
                               self->login_token, on_ap_login_result, self);
}

/* ── Worker thread ───────────────────────────────────────────────────────── */

static gboolean
begin_signin (gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  self->ap = spotifygtk_ap_session_new ();
  spotifygtk_ap_session_set_cancellable (self->ap, self->cancellable);
  spotifygtk_ap_session_connect (self->ap, NULL, on_ap_connected, self);

  return G_SOURCE_REMOVE;
}

static gpointer
session_thread (gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  g_main_context_push_thread_default (self->context);

  /* Token acquisition can block (it may run a browser flow), so it happens
   * here rather than on the caller's thread. */
  NativeAuth *auth = native_auth_new ();
  if (native_auth_has_valid_token (auth)) {
    self->login_token = g_strdup (native_auth_get_token (auth));
  } else {
    /* native_auth_refresh() handles both cases: refresh a stored-but-expired
     * token, or fall through to the browser flow when there is none. It is
     * async, so drive it on this thread's context via its own nested loop. */
    GMainLoop *token_loop = g_main_loop_new (self->context, FALSE);
    gulong handler = g_signal_connect_swapped (auth, "completed",
                                               G_CALLBACK (g_main_loop_quit), token_loop);
    native_auth_refresh (auth);
    g_main_loop_run (token_loop);
    g_signal_handler_disconnect (auth, handler);
    g_main_loop_unref (token_loop);

    if (native_auth_has_valid_token (auth))
      self->login_token = g_strdup (native_auth_get_token (auth));
  }
  g_object_unref (auth);

  if (!self->login_token) {
    fail_session (self, "could not obtain an access token for AP login");
    g_main_context_pop_thread_default (self->context);
    return NULL;
  }

  g_main_context_invoke (self->context, begin_signin, self);

  /* Publish the loop under the lock and re-check `stopping` in the same
   * critical section. Without this, dispose() can run between "loop is
   * still NULL, nothing to quit" and this thread entering g_main_loop_run,
   * and then g_thread_join blocks forever on a loop nobody will ever quit. */
  g_mutex_lock (&self->lock);
  if (self->stopping) {
    g_mutex_unlock (&self->lock);
    g_main_context_pop_thread_default (self->context);
    return NULL;
  }
  self->loop = g_main_loop_new (self->context, FALSE);
  GMainLoop *loop = g_main_loop_ref (self->loop);
  g_mutex_unlock (&self->lock);

  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  g_main_context_pop_thread_default (self->context);
  return NULL;
}

void
spotifygtk_native_session_start (SpotifyNativeSession *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));

  if (self->thread)
    return;   /* already connecting or ready */

  set_state (self, SPOTIFYGTK_SESSION_CONNECTING, "Connecting to Spotify…");

  self->context = g_main_context_new ();
  self->thread  = g_thread_new ("spotify-session", session_thread, self);
}

/* ── Catalog: context resolve + batched metadata ─────────────────────────── */

typedef struct {
  SpotifyNativeSession *session;   /* not owned; the GTask holds the ref */
  GTask                *task;
  gchar                *context_uri;
  guint                 max_tracks;

  /* Paging state. A context resolve hands back every track URI at once, but a
   * metadata request for all of them is refused outright once the list is big
   * enough (4773 rejected, 2000 accepted). So the URIs are kept here and walked
   * in SPOTIFYGTK_SESSION_MAX_BATCH-sized requests, accumulating into `out`. */
  GPtrArray            *uris;      /* gchar*, owned; the full list */
  guint                 next_uri;  /* start index of the request in flight */
  GPtrArray            *out;       /* SpotifyNativeTrack*, owned; accumulated */
} LoadTracksOp;

static void
load_op_free (LoadTracksOp *op)
{
  g_free (op->context_uri);
  g_clear_pointer (&op->uris, g_ptr_array_unref);
  g_clear_pointer (&op->out, g_ptr_array_unref);
  g_free (op);
}

/* Hand the accumulated tracks back and finish the task. */
static void
load_op_finish (LoadTracksOp *op)
{
  GPtrArray *out = g_steal_pointer (&op->out);
  g_task_return_pointer (op->task, out, (GDestroyNotify) g_ptr_array_unref);
  g_object_unref (op->task);
  load_op_free (op);
}

static void request_next_page (LoadTracksOp *op);

static void
on_batch_metadata (const SpclientTrackInfo *tracks, guint n_tracks,
                   GError *error, gpointer user_data)
{
  LoadTracksOp *op = user_data;

  if (error || !tracks) {
    /* A page that fails after earlier ones succeeded still leaves something
     * worth showing, and a partial collection beats an error page -- so only
     * fail the whole load when nothing at all was gathered. */
    if (op->out && op->out->len > 0) {
      g_warning ("session: metadata page starting at %u failed (%s); returning "
                 "the %u track(s) already loaded",
                 op->next_uri, error ? error->message : "no tracks returned",
                 op->out->len);
      load_op_finish (op);
      return;
    }
    g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_FAILED,
      "could not load track metadata: %s",
      error ? error->message : "no tracks returned");
    g_object_unref (op->task);
    load_op_free (op);
    return;
  }

  GPtrArray *out = op->out;

  for (guint i = 0; i < n_tracks; i++) {
    const SpclientTrackInfo *info = &tracks[i];
    SpotifyNativeTrack *track = g_new0 (SpotifyNativeTrack, 1);

    track->uri         = g_strdup (info->entity_uri);
    track->name        = g_strdup (info->meta.name);
    track->artists     = g_strdup (info->meta.artist_names);
    track->album       = g_strdup (info->meta.album_name);
    track->duration_ms = info->meta.duration_ms;
    track->is_explicit = info->meta.is_explicit;
    track->cover_id    = g_strdup (info->meta.cover_id);
    track->album_uri   = g_strdup (info->meta.album_uri);
    track->artist_uri  = g_strdup (info->meta.artist_uri);

    g_ptr_array_add (out, track);
  }

  op->next_uri += n_tracks;
  request_next_page (op);
}

/*
 * Request metadata for the next chunk, or finish if the list is exhausted.
 *
 * Sequential rather than parallel on purpose: the requests share one bearer and
 * one spclient, and a collection is three or four chunks at most, so overlapping
 * them buys little and makes cancellation and partial failure much harder to
 * reason about.
 */
static void
request_next_page (LoadTracksOp *op)
{
  if (!op->uris || op->next_uri >= op->uris->len) {
    load_op_finish (op);
    return;
  }

  guint remaining = op->uris->len - op->next_uri;
  guint n         = MIN (remaining, SPOTIFYGTK_SESSION_MAX_BATCH);

  g_mutex_lock (&op->session->lock);
  g_autofree gchar *bearer = g_strdup (op->session->bearer_token);
  g_autofree gchar *ctoken = g_strdup (op->session->client_token);
  g_mutex_unlock (&op->session->lock);

  if (op->uris->len > SPOTIFYGTK_SESSION_MAX_BATCH)
    g_message ("session: metadata page %u-%u of %u",
               op->next_uri, op->next_uri + n, op->uris->len);

  /* The chunk is a pointer slice into the existing array -- the URI strings are
   * borrowed for the duration of the call, not copied. */
  spotifygtk_spclient_get_tracks_metadata (op->session->spclient,
                                           (const gchar *const *) &op->uris->pdata[op->next_uri],
                                           n, bearer, ctoken,
                                           on_batch_metadata, op);
}

static void
on_context_resolved (JsonNode *context, GError *error, gpointer user_data)
{
  LoadTracksOp *op = user_data;

  if (error || !context) {
    g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_FAILED,
      "could not resolve context: %s",
      error ? error->message : "no data returned");
    g_object_unref (op->task);
    load_op_free (op);
    return;
  }

  /* Context → flat list of track URIs. ContextTrack carries only `uri`,
   * so this is all there is to extract; display metadata is the second
   * request below. */
  GPtrArray *uris = g_ptr_array_new_with_free_func (g_free);

  if (JSON_NODE_HOLDS_OBJECT (context)) {
    JsonObject *root = json_node_get_object (context);
    if (json_object_has_member (root, "pages")) {
      JsonArray *pages = json_object_get_array_member (root, "pages");
      for (guint p = 0; pages && p < json_array_get_length (pages); p++) {
        JsonObject *page = json_array_get_object_element (pages, p);
        if (!page || !json_object_has_member (page, "tracks"))
          continue;

        JsonArray *tracks = json_object_get_array_member (page, "tracks");
        for (guint t = 0; tracks && t < json_array_get_length (tracks); t++) {
          JsonObject *entry = json_array_get_object_element (tracks, t);
          if (!entry || !json_object_has_member (entry, "uri"))
            continue;
          if (uris->len >= op->max_tracks)
            break;
          g_ptr_array_add (uris, g_strdup (json_object_get_string_member (entry, "uri")));
        }
        if (uris->len >= op->max_tracks)
          break;
      }
    }
  }

  json_node_unref (context);

  if (uris->len == 0) {
    /* An empty context is a legitimate answer (no search results, empty
     * playlist), so return an empty array rather than an error and let the
     * UI say "nothing here". */
    g_ptr_array_unref (uris);
    GPtrArray *empty = g_ptr_array_new_with_free_func (
      (GDestroyNotify) spotifygtk_native_track_free);
    g_task_return_pointer (op->task, empty, (GDestroyNotify) g_ptr_array_unref);
    g_object_unref (op->task);
    load_op_free (op);
    return;
  }

  /* Hand the list to the op and walk it a page at a time. No NULL terminator:
   * get_tracks_metadata() takes an explicit count, which is what lets a page be
   * a plain pointer slice into this array rather than a copy. */
  op->uris     = uris;
  op->next_uri = 0;
  op->out      = g_ptr_array_new_with_free_func (
                   (GDestroyNotify) spotifygtk_native_track_free);

  request_next_page (op);
}

/* Issue the context-resolve for an op whose bearer is known good. */
static void
run_load_op (LoadTracksOp *op)
{
  SpotifyNativeSession *self = op->session;

  g_mutex_lock (&self->lock);
  g_autofree gchar *bearer = g_strdup (self->bearer_token);
  g_autofree gchar *ctoken = g_strdup (self->client_token);
  g_mutex_unlock (&self->lock);

  spotifygtk_spclient_get_context (self->spclient, op->context_uri,
                                   bearer, ctoken,
                                   on_context_resolved, op);
}

/* Called when a refresh settles: run everything that was waiting, or fail it
 * all if the refresh could not produce a usable bearer. */
static void
flush_pending_ops (SpotifyNativeSession *self, gboolean refresh_ok)
{
  GList *ops = self->pending_ops;
  self->pending_ops = NULL;

  for (GList *l = ops; l; l = l->next) {
    LoadTracksOp *op = l->data;

    if (refresh_ok) {
      run_load_op (op);
    } else {
      g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "session credentials expired and could not be renewed");
      g_object_unref (op->task);
      load_op_free (op);
    }
  }

  g_list_free (ops);
}

static gboolean
start_load_tracks (gpointer user_data)
{
  LoadTracksOp *op = user_data;
  SpotifyNativeSession *self = op->session;

  g_mutex_lock (&self->lock);
  gboolean ready = (self->state == SPOTIFYGTK_SESSION_READY);
  gint64   expires_at = self->bearer_expires_at;
  g_mutex_unlock (&self->lock);

  if (!ready || !self->spclient) {
    g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                             "session is not signed in yet");
    g_object_unref (op->task);
    load_op_free (op);
    return G_SOURCE_REMOVE;
  }

  /* Re-mint before use rather than after a 401: the failure mode otherwise
   * is a session that still says READY while every request is rejected. */
  gboolean stale = expires_at > 0 &&
                   g_get_monotonic_time () > expires_at - BEARER_REFRESH_MARGIN_US;

  if (stale || self->refreshing) {
    self->pending_ops = g_list_append (self->pending_ops, op);
    if (!self->refreshing)
      start_bearer_refresh (self);
    return G_SOURCE_REMOVE;
  }

  run_load_op (op);
  return G_SOURCE_REMOVE;
}

void
spotifygtk_native_session_load_tracks (SpotifyNativeSession *self,
                                       const gchar          *context_uri,
                                       guint                 max_tracks,
                                       GCancellable         *cancellable,
                                       GAsyncReadyCallback   callback,
                                       gpointer              user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));
  g_return_if_fail (context_uri != NULL);

  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, spotifygtk_native_session_load_tracks);

  if (max_tracks == 0)
    max_tracks = SPOTIFYGTK_SESSION_DEFAULT_MAX_TRACKS;
  /* Clamped to the overall ceiling, not to one request's worth: anything larger
   * than a single batch is paged rather than truncated. */
  max_tracks = MIN (max_tracks, SPOTIFYGTK_SESSION_MAX_TRACKS);

  if (!self->context) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                             "session has not been started");
    g_object_unref (task);
    return;
  }

  LoadTracksOp *op = g_new0 (LoadTracksOp, 1);
  op->session     = self;
  op->task        = task;   /* ref transferred; holds a ref on `self` too */
  op->context_uri = g_strdup (context_uri);
  op->max_tracks  = max_tracks;

  /* Hop to the worker: every spclient call must run on its context. */
  g_main_context_invoke (self->context, start_load_tracks, op);
}

GPtrArray *
spotifygtk_native_session_load_tracks_finish (SpotifyNativeSession *self,
                                              GAsyncResult         *result,
                                              GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

SpotifyNativeSessionState
spotifygtk_native_session_get_state (SpotifyNativeSession *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self), SPOTIFYGTK_SESSION_FAILED);

  g_mutex_lock (&self->lock);
  SpotifyNativeSessionState state = self->state;
  g_mutex_unlock (&self->lock);
  return state;
}

gchar *
spotifygtk_native_session_dup_username (SpotifyNativeSession *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self), NULL);

  g_mutex_lock (&self->lock);
  gchar *username = g_strdup (self->username);
  g_mutex_unlock (&self->lock);
  return username;
}

gchar *
spotifygtk_native_session_dup_collection_uri (SpotifyNativeSession *self)
{
  g_autofree gchar *username = spotifygtk_native_session_dup_username (self);
  if (!username)
    return NULL;
  return spotifygtk_spclient_build_collection_uri (username);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

static void
spotifygtk_native_session_dispose (GObject *object)
{
  SpotifyNativeSession *self = SPOTIFYGTK_NATIVE_SESSION (object);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  /* Claim the loop under the lock so the worker cannot start one after this
   * point — see the matching comment in session_thread(). */
  g_mutex_lock (&self->lock);
  self->stopping = TRUE;
  GMainLoop *loop = self->loop ? g_main_loop_ref (self->loop) : NULL;
  g_mutex_unlock (&self->lock);

  if (loop) {
    g_main_loop_quit (loop);
    g_main_loop_unref (loop);
  }

  if (self->thread) {
    g_thread_join (self->thread);
    self->thread = NULL;
  }

  /* The worker has stopped, so anything still queued behind a refresh will
   * never run. Fail those tasks rather than leaking them and leaving their
   * callers waiting forever. */
  for (GList *l = self->pending_ops; l; l = l->next) {
    LoadTracksOp *op = l->data;
    g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "session shut down");
    g_object_unref (op->task);
    load_op_free (op);
  }
  g_clear_pointer (&self->pending_ops, g_list_free);

  g_clear_pointer (&self->loop, g_main_loop_unref);
  g_clear_pointer (&self->context, g_main_context_unref);
  g_clear_pointer (&self->caller_context, g_main_context_unref);

  g_clear_object (&self->ap);
  g_clear_object (&self->client_token_client);
  g_clear_object (&self->login5_client);
  g_clear_object (&self->spclient);
  g_clear_object (&self->cancellable);

  g_clear_pointer (&self->device_id, g_free);
  g_clear_pointer (&self->login_token, g_free);
  g_clear_pointer (&self->bearer_token, g_free);
  g_clear_pointer (&self->client_token, g_free);
  g_clear_pointer (&self->username, g_free);

  G_OBJECT_CLASS (spotifygtk_native_session_parent_class)->dispose (object);
}

static void
spotifygtk_native_session_finalize (GObject *object)
{
  SpotifyNativeSession *self = SPOTIFYGTK_NATIVE_SESSION (object);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (spotifygtk_native_session_parent_class)->finalize (object);
}

static void
spotifygtk_native_session_class_init (SpotifyNativeSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose  = spotifygtk_native_session_dispose;
  object_class->finalize = spotifygtk_native_session_finalize;

  signals[STATE_CHANGED] = g_signal_new ("state-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);
}

static void
spotifygtk_native_session_init (SpotifyNativeSession *self)
{
  g_mutex_init (&self->lock);
  self->state       = SPOTIFYGTK_SESSION_IDLE;
  self->cancellable = g_cancellable_new ();
  self->device_id   = generate_device_id ();

  self->caller_context = g_main_context_ref_thread_default ();
}

SpotifyNativeSession *
spotifygtk_native_session_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_NATIVE_SESSION, NULL);
}

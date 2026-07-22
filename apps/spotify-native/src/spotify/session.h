/*
 * session.h — A long-lived authenticated Spotify session for the UI.
 *
 * WHY THIS EXISTS
 *
 * The client-token → login5 → spclient auth-relay works, but until now it
 * only existed inside main.c's one-shot playback pipeline: built at the
 * start of a playback attempt and torn down at the end. A UI needs the
 * opposite lifetime — sign in once, then answer many catalog queries — so
 * this holds the same chain open and serves requests against it.
 *
 * It is written against the existing public modules (ap.h, clienttoken.h,
 * login5.h, spclient.h) rather than by refactoring main.c, so the proven
 * playback harness is untouched by any of this.
 *
 * THREADING
 *
 * The session owns a worker thread running its own GMainContext, for the
 * same reason run_live_test() does: AP, libsoup and timeout sources must
 * never acquire GTK's default context and dispatch protocol callbacks on
 * the UI thread.
 *
 * The public API is main-thread-facing. Requests are handed to the worker
 * internally; results come back on the context the request was made from,
 * via GTask, so callers write ordinary async GIO code and never see the
 * worker.
 *
 * WHAT A CATALOG QUERY ACTUALLY COSTS
 *
 * Two round trips, because /context-resolve returns bare track URIs and
 * nothing else — no titles, no artists (confirmed live: ContextTrack has
 * only a `uri` field). load_tracks() does both halves: resolve the context,
 * then batch-resolve the URIs to display metadata in a single second
 * request. Callers should not try to assemble this themselves.
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
  SPOTIFYGTK_SESSION_IDLE,
  SPOTIFYGTK_SESSION_CONNECTING,
  SPOTIFYGTK_SESSION_READY,
  SPOTIFYGTK_SESSION_FAILED,
} SpotifyNativeSessionState;

/* One renderable track. Owns its strings; free with
 * spotifygtk_native_track_free(). A NULL name/artists/album means the
 * server did not supply one — rendering a placeholder is the UI's call. */
typedef struct {
  gchar   *uri;
  gchar   *name;
  gchar   *artists;      /* ", "-joined */
  gchar   *album;
  gint64   duration_ms;
  gboolean is_explicit;
  gchar   *cover_id;     /* hex Image.file_id; NULL if the album has no art */
  gchar   *album_uri;    /* spotify:album:<id>;  NULL if unknown */
  gchar   *artist_uri;   /* spotify:artist:<id> (primary); NULL if unknown */
} SpotifyNativeTrack;

void spotifygtk_native_track_free (SpotifyNativeTrack *track);

/* Deep copy; free the result with spotifygtk_native_track_free(). NULL in,
 * NULL out. The UI keeps its own play-context and queue copies of tracks,
 * whose lifetimes are independent of the GPtrArray a load returned. */
SpotifyNativeTrack *spotifygtk_native_track_copy (const SpotifyNativeTrack *track);

#define SPOTIFYGTK_TYPE_NATIVE_SESSION (spotifygtk_native_session_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyNativeSession, spotifygtk_native_session,
                      SPOTIFYGTK, NATIVE_SESSION, GObject)

SpotifyNativeSession *spotifygtk_native_session_new (void);

/*
 * Begin signing in. Returns immediately; watch "state-changed".
 * Calling this while already connecting or ready is a no-op.
 */
void spotifygtk_native_session_start (SpotifyNativeSession *self);

SpotifyNativeSessionState spotifygtk_native_session_get_state (SpotifyNativeSession *self);

/* Canonical username from APWelcome. NULL until the session is READY.
 * Returns a copy — the worker thread owns the original. */
gchar *spotifygtk_native_session_dup_username (SpotifyNativeSession *self);

/* Build the liked-songs context URI for the signed-in user, or NULL if
 * the session is not READY yet. */
gchar *spotifygtk_native_session_dup_collection_uri (SpotifyNativeSession *self);

/*
 * Resolve a context URI to renderable tracks (both round trips).
 *
 * `context_uri` accepts anything /context-resolve takes: a search URI
 * from spotifygtk_spclient_build_search_uri(), a collection URI, or a
 * playlist/album/artist URI.
 *
 * `max_tracks` caps how many URIs are resolved to metadata. This is not
 * optional politeness: a real library is thousands of tracks and a single
 * batch that large is rejected by the server. 0 means "use the default".
 *
 * Finish returns a GPtrArray of SpotifyNativeTrack* with a free func set;
 * unref it when done.
 */
void spotifygtk_native_session_load_tracks (SpotifyNativeSession *self,
                                            const gchar          *context_uri,
                                            guint                 max_tracks,
                                            GCancellable         *cancellable,
                                            GAsyncReadyCallback   callback,
                                            gpointer              user_data);

GPtrArray *spotifygtk_native_session_load_tracks_finish (SpotifyNativeSession *self,
                                                         GAsyncResult         *result,
                                                         GError              **error);

/* Default and ceiling for max_tracks. The server accepts batches well
 * above DEFAULT (2000 confirmed working, all 4773 of a real collection
 * rejected), but a UI page has no reason to ask for more. */
#define SPOTIFYGTK_SESSION_DEFAULT_MAX_TRACKS 200
#define SPOTIFYGTK_SESSION_MAX_BATCH          500  /* one server batch; confirmed working at 500/1000/2000 */

/* Signal: state-changed (SpotifyNativeSessionState state, const gchar *message) */

G_END_DECLS

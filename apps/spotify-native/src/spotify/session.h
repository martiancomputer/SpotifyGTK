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
#include "mercury.h"

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
  gchar   *cover_id;       /* hex Image.file_id, widest variant; NULL if no art */
  /* Narrowest variant of the same art. A row thumbnail is 96px; fetching the
   * 640px original for it cost 112 KB a row. */
  gchar   *cover_id_small;
  gchar   *album_uri;    /* spotify:album:<id>;  NULL if unknown */
  gchar   *artist_uri;   /* spotify:artist:<id> (primary); NULL if unknown */
  gint     release_year; /* from the album; 0 when unknown */
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

/*
 * Drop the connection and sign in again.
 *
 * start() returns early once its thread exists, so there was no way back from
 * a dropped AP link: everything riding on it failed silently for the life of
 * the process.
 */
void spotifygtk_native_session_reconnect (SpotifyNativeSession *self);

SpotifyNativeSessionState spotifygtk_native_session_get_state (SpotifyNativeSession *self);

/* Canonical username from APWelcome. NULL until the session is READY.
 * Returns a copy — the worker thread owns the original. */
gchar *spotifygtk_native_session_dup_username (SpotifyNativeSession *self);

/* Build the liked-songs context URI for the signed-in user, or NULL if
 * the session is not READY yet. */
gchar *spotifygtk_native_session_dup_collection_uri (SpotifyNativeSession *self);

/*
 * Mercury on this session's AP connection, created on first use.
 *
 * The engine builds its own for the duration of a track, which meant anything
 * needing Mercury -- reading Liked Songs, writing one -- could not happen until
 * something had played. This one exists from sign-in.
 *
 * NULL before the session is READY. Owned by the session.
 */
SpotifyMercury *spotifygtk_native_session_get_mercury (SpotifyNativeSession *self);

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

/*
 * The artist's portrait image id, for the artist page's banner.
 *
 * A track carries its album's cover and nothing else, so this is the only way
 * to show the artist rather than one of their releases. Runs on the session's
 * worker like every other request here; the callback arrives on the caller's
 * context. `cover_id` is NULL when the artist has no portrait.
 */
typedef void (*SpotifyNativeArtistImageFunc) (const gchar *cover_id,
                                              gpointer     user_data);

void spotifygtk_native_session_get_artist_image (SpotifyNativeSession *self,
                                                 const gchar          *artist_uri,
                                                 SpotifyNativeArtistImageFunc callback,
                                                 gpointer              user_data);

/*
 * Drop the cached resolution of a context, so the next load of it goes to the
 * network.
 *
 * The collection is cached for the life of the session, which is right for
 * navigation -- returning to Liked Songs should not refetch thousands of
 * tracks -- but wrong after the collection has been written to. Without this,
 * a caller that marks its page stale still gets served the pre-write set and
 * concludes nothing changed. Pass NULL to drop whatever is cached.
 */
void spotifygtk_native_session_invalidate_context (SpotifyNativeSession *self,
                                                   const gchar          *context_uri);

/* Default and ceiling for max_tracks.
 *
 * The ceiling is the largest batch the server has actually been observed to
 * accept: 500, 1000 and 2000 all resolve, 4773 (a whole real collection) is
 * rejected. It sat at 1000 while every page asked for 1000, so the pages were
 * silently pinned to half of what works. Raised to the measured limit rather
 * than to a round number -- going past it does not fail gracefully, the whole
 * request is refused and the page shows nothing.
 *
 * MAX_BATCH is one request's worth. A load larger than that is split into
 * several sequential requests and the results concatenated, so MAX_TRACKS --
 * the ceiling on a whole load -- is independent of it and exists only to stop
 * a pathological context from being fetched in its entirety. */
#define SPOTIFYGTK_SESSION_DEFAULT_MAX_TRACKS 200
#define SPOTIFYGTK_SESSION_MAX_BATCH          2000  /* one request; 500/1000/2000 confirmed, 4773 rejected */
#define SPOTIFYGTK_SESSION_MAX_TRACKS         10000 /* whole load, across pages */

/* Signal: state-changed (SpotifyNativeSessionState state, const gchar *message) */

G_END_DECLS

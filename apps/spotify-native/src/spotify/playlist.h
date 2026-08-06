/*
 * playlist.h — Creating playlists and adding tracks, over Mercury.
 *
 * All three operations are proven against a live account:
 *
 *   POST hm://playlist/v2/playlist                 create
 *   POST hm://playlist/v2/user/<user>/rootlist/changes   file into the library
 *   POST hm://playlist/v2/playlist/<id>/changes    add tracks
 *
 * Two things about this service differ from the collection one and are easy to
 * get wrong.
 *
 * POST, not PUT. The collection accepts only PUT and answers 405 to POST; this
 * service is the exact reverse. The verb is per-service.
 *
 * A created playlist is not in the user's library. Creating the list and
 * filing it into the rootlist are separate calls, so a create that returns 200
 * still leaves nothing on screen until the second one runs.
 *
 * Changes are revision-checked: an op names the revision it was based on and
 * is rejected if the list has moved on. So every write here is read-then-write
 * -- fetch the head for its revision, then post against it -- rather than the
 * fire-and-forget a collection write gets.
 */

#pragma once

#include <glib.h>
#include "mercury.h"

G_BEGIN_DECLS

/* ok is TRUE only for a 2xx. `uri` is the new playlist on success, NULL
 * otherwise, and is owned by the callee. */
typedef void (*SpotifyPlaylistCreateCallback) (gboolean     ok,
                                               gint32       status,
                                               const gchar *uri,
                                               gpointer     user_data);

typedef void (*SpotifyPlaylistOpCallback) (gboolean ok, gint32 status,
                                           gpointer user_data);

/*
 * Create a playlist and file it into the user's library.
 *
 * Both steps, because a playlist that exists but is in no rootlist is
 * invisible and there is no reason for a caller to want that.
 */
void spotifygtk_playlist_create (SpotifyMercury                *mercury,
                                 const gchar                   *username,
                                 const gchar                   *name,
                                 SpotifyPlaylistCreateCallback  callback,
                                 gpointer                       user_data);

/*
 * Append tracks to a playlist. `playlist_uri` may be a spotify:playlist:<id>
 * URI or a bare id.
 *
 * Reads the list's head first for its revision, so this is two round trips.
 */
void spotifygtk_playlist_add_tracks (SpotifyMercury            *mercury,
                                     const gchar               *playlist_uri,
                                     const gchar *const        *track_uris,
                                     guint                      n_tracks,
                                     SpotifyPlaylistOpCallback  callback,
                                     gpointer                   user_data);

/* One entry of the user's rootlist. */
typedef struct {
  gchar *uri;
  gchar *name;
} SpotifyPlaylistEntry;

void spotifygtk_playlist_entries_free (SpotifyPlaylistEntry *entries, guint n);

/* Entries are owned by the callee and valid for the callback's duration. */
typedef void (*SpotifyPlaylistListCallback) (gboolean              ok,
                                             gint32                status,
                                             SpotifyPlaylistEntry *entries,
                                             guint                 n_entries,
                                             gpointer              user_data);

/*
 * The user's playlists.
 *
 * The rootlist carries URIs but not names, so each name is a further lookup;
 * this returns the URIs immediately with `name` NULL, and callers that need
 * names resolve them separately rather than waiting on a fan-out here.
 */
void spotifygtk_playlist_list (SpotifyMercury              *mercury,
                               const gchar                 *username,
                               SpotifyPlaylistListCallback  callback,
                               gpointer                     user_data);

G_END_DECLS

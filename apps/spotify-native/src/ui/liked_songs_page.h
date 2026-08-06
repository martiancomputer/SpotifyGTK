/*
 * liked_songs_page.h — Liked Songs, via the native session.
 *
 * Backed by the `spotify:user:<id>:collection` context URI rather than the
 * Web API's /me/tracks. See spotify/session.h.
 */

#pragma once

#include <adwaita.h>

#include "spotify/session.h"
#include "track_list.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_LIKED_SONGS_PAGE (spotifygtk_liked_songs_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkLikedSongsPage, spotifygtk_liked_songs_page,
                      SPOTIFYGTK, LIKED_SONGS_PAGE, GtkBox)

SpotifyGtkLikedSongsPage *spotifygtk_liked_songs_page_new (void);

void spotifygtk_liked_songs_page_set_session (SpotifyGtkLikedSongsPage *self,
                                              SpotifyNativeSession     *session);
/* Borrow the window's set of liked URIs, used to drop rows the collection no
 * longer lists. See the implementation for why this is needed. */
void spotifygtk_liked_songs_page_set_liked_filter (SpotifyGtkLikedSongsPage *self,
                                                   GHashTable *liked_uris);

/* Force the next refresh to refetch, after the collection has changed. Also
 * drops the session's cached listing, without which the refetch is served from
 * memory and returns the pre-change set. */
void spotifygtk_liked_songs_page_invalidate (SpotifyGtkLikedSongsPage *self);

/*
 * Reflect a single like or unlike immediately, without refetching.
 *
 * Both update the loaded collection rather than just the visible rows, so the
 * change survives a filter keystroke or a re-sort. Adding puts the track at
 * the top, which is where the server will have it under the default ordering.
 *
 * No-ops on a page that has never loaded: it will fetch the truth when opened,
 * and a page holding one row would misrepresent the library. Being merely
 * stale is not a reason to skip -- the caller invalidates after every one.
 */
void spotifygtk_liked_songs_page_add_track (SpotifyGtkLikedSongsPage *self,
                                            const SpotifyNativeTrack *track);
void spotifygtk_liked_songs_page_remove_track (SpotifyGtkLikedSongsPage *self,
                                               const gchar *uri);

void spotifygtk_liked_songs_page_refresh (SpotifyGtkLikedSongsPage *self);

/* The track list, for play-context and the row context menu — see
 * spotifygtk_search_page_get_list(). */
SpotifyGtkTrackList *spotifygtk_liked_songs_page_get_list (SpotifyGtkLikedSongsPage *self);

void spotifygtk_liked_songs_page_set_playing_uri (SpotifyGtkLikedSongsPage *self,
                                          const gchar *uri,
                                          gboolean playing);

/* Signals:
 * - track-activated (SpotifyNativeTrack *track)
 */

G_END_DECLS

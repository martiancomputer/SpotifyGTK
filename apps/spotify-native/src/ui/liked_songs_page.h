/*
 * liked_songs_page.h — Liked Songs, via the native session.
 *
 * Backed by the `spotify:user:<id>:collection` context URI rather than the
 * Web API's /me/tracks. See spotify/session.h.
 */

#pragma once

#include <adwaita.h>

#include "spotify/session.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_LIKED_SONGS_PAGE (spotifygtk_liked_songs_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkLikedSongsPage, spotifygtk_liked_songs_page,
                      SPOTIFYGTK, LIKED_SONGS_PAGE, GtkBox)

SpotifyGtkLikedSongsPage *spotifygtk_liked_songs_page_new (void);

void spotifygtk_liked_songs_page_set_session (SpotifyGtkLikedSongsPage *self,
                                              SpotifyNativeSession     *session);
void spotifygtk_liked_songs_page_refresh (SpotifyGtkLikedSongsPage *self);

/* Signals:
 * - track-activated (SpotifyNativeTrack *track)
 */

G_END_DECLS

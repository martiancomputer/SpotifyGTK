/*
 * search_page.h — Catalog search page.
 *
 * Backed by the native session (spclient context-resolve + batched
 * extended metadata), not the Web API. See spotify/session.h for why.
 */

#pragma once

#include <adwaita.h>

#include "spotify/session.h"
#include "track_list.h"
#include "album_grid.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_SEARCH_PAGE (spotifygtk_search_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkSearchPage, spotifygtk_search_page,
                      SPOTIFYGTK, SEARCH_PAGE, GtkBox)

SpotifyGtkSearchPage *spotifygtk_search_page_new (void);

void spotifygtk_search_page_set_session (SpotifyGtkSearchPage *self,
                                         SpotifyNativeSession *session);

/* The results list, so the window can wire play-context and the row context
 * menu (add-to-queue / go-to-album / go-to-artist) to it directly. */
SpotifyGtkTrackList *spotifygtk_search_page_get_list (SpotifyGtkSearchPage *self);

/* The albums shelf, so the window can wire "album-activated" to the context
 * page exactly as it wires the row menu's "go-to-album". */
SpotifyGtkAlbumGrid *spotifygtk_search_page_get_album_grid (SpotifyGtkSearchPage *self);

void spotifygtk_search_page_set_playing_uri (SpotifyGtkSearchPage *self,
                                          const gchar *uri,
                                          gboolean playing);

/* Signals:
 * - track-activated (SpotifyNativeTrack *track)
 */

G_END_DECLS

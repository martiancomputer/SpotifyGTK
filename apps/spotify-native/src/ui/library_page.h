/*
 * library_page.h — Library page.
 *
 * The playlist list needs spclient's rootlist endpoint, which returns
 * playlist4_external protobuf rather than the JSON the rest of the catalog
 * path uses. Until that parser exists this page says so, rather than
 * falling back to api.spotify.com — see the README status table for why
 * that path is unusable.
 */

#pragma once

#include <adwaita.h>

#include "spotify/session.h"
#include "album_grid.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_LIBRARY_PAGE (spotifygtk_library_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkLibraryPage, spotifygtk_library_page,
                      SPOTIFYGTK, LIBRARY_PAGE, GtkBox)

SpotifyGtkLibraryPage *spotifygtk_library_page_new (void);

/* Once the session is READY, fill the Albums grid from the collection. */
void spotifygtk_library_page_set_session (SpotifyGtkLibraryPage *self,
                                          SpotifyNativeSession  *session);

/* The albums grid, so the window can wire "album-activated" to context nav. */
SpotifyGtkAlbumGrid *spotifygtk_library_page_get_album_grid (SpotifyGtkLibraryPage *self);

G_END_DECLS

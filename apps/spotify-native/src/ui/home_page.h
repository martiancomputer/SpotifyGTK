#pragma once
#include <adwaita.h>
#include "spotify/session.h"
#include "album_grid.h"
G_BEGIN_DECLS
#define SPOTIFYGTK_TYPE_HOME_PAGE (spotifygtk_home_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkHomePage, spotifygtk_home_page,
                      SPOTIFYGTK, HOME_PAGE, GtkBox)
SpotifyGtkHomePage *spotifygtk_home_page_new (void);
void spotifygtk_home_page_set_session (SpotifyGtkHomePage *self, SpotifyNativeSession *session);
/* Display-only data is copied into bounded card models, without hydration. */
void spotifygtk_home_page_set_feed (SpotifyGtkHomePage *self, const SpotifyHomeFeed *feed);
GPtrArray *spotifygtk_home_page_get_grids (SpotifyGtkHomePage *self); /* borrowed */
void spotifygtk_home_page_set_covers_loaded (SpotifyGtkHomePage *self, gboolean loaded);
void spotifygtk_home_page_clear_cache (SpotifyGtkHomePage *self);
/* Signals: loading-changed (boolean), grid-added (SpotifyGtkAlbumGrid),
 * context-requested (URI, title, play), destination-requested (page name). */
G_END_DECLS

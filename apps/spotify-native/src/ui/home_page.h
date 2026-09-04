/*
 * home_page.h — Home page.
 *
 * The mockup's two sections need endpoints the native path does not have
 * yet: "Recently Played" has no spclient equivalent (librespot exposes
 * none), and "Continue Listening" needs the playlist rootlist, which is
 * playlist4_external protobuf rather than the JSON everything else returns.
 *
 * Rather than keep this page on api.spotify.com — which fails with a
 * shared-quota 429, see the README status table — it states what is
 * missing. An empty page that explains itself beats a populated one that
 * only works when a globally-contended quota happens to be free.
 */

#pragma once

#include <adwaita.h>

#include "spotify/session.h"
#include "album_grid.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_HOME_PAGE (spotifygtk_home_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkHomePage, spotifygtk_home_page,
                      SPOTIFYGTK, HOME_PAGE, GtkBox)

SpotifyGtkHomePage *spotifygtk_home_page_new (void);

/* Once the session is READY, load the collection and fill the "From your
 * Liked Songs" shelf. A no-op until then. */
void spotifygtk_home_page_set_session (SpotifyGtkHomePage   *self,
                                       SpotifyNativeSession *session);

/* The shelf, so the window can wire "album-activated" to the context page. */
SpotifyGtkAlbumGrid *spotifygtk_home_page_get_album_grid (SpotifyGtkHomePage *self);

/* Signals:
 * - loading-changed (gboolean loading)
 */

G_END_DECLS

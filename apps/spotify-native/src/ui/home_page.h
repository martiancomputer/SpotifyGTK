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

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_HOME_PAGE (spotifygtk_home_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkHomePage, spotifygtk_home_page,
                      SPOTIFYGTK, HOME_PAGE, GtkBox)

SpotifyGtkHomePage *spotifygtk_home_page_new (void);

G_END_DECLS

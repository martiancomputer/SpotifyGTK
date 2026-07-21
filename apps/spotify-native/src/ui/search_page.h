/*
 * search_page.h — Catalog search page.
 *
 * Backed by the native session (spclient context-resolve + batched
 * extended metadata), not the Web API. See spotify/session.h for why.
 */

#pragma once

#include <adwaita.h>

#include "spotify/session.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_SEARCH_PAGE (spotifygtk_search_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkSearchPage, spotifygtk_search_page,
                      SPOTIFYGTK, SEARCH_PAGE, GtkBox)

SpotifyGtkSearchPage *spotifygtk_search_page_new (void);

void spotifygtk_search_page_set_session (SpotifyGtkSearchPage *self,
                                         SpotifyNativeSession *session);

/* Signals:
 * - track-activated (SpotifyNativeTrack *track)
 */

G_END_DECLS

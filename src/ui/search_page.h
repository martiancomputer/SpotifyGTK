#pragma once
#include <adwaita.h>
#include "../api.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_SEARCH_PAGE (spotifygtk_search_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifySearchPage, spotifygtk_search_page,
                      SPOTIFYGTK, SEARCH_PAGE, AdwBin)

SpotifySearchPage *spotifygtk_search_page_new (void);
void spotifygtk_search_page_set_api (SpotifySearchPage *self, SpotifyApi *api);

G_END_DECLS
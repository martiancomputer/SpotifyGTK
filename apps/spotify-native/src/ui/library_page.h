/*
 * library_page.h — Library page.
 *
 * Saved releases, local albums, and optionally playlists share one virtualized
 * card grid. The window owns the rootlist request and supplies playlist cards.
 */

#pragma once

#include <adwaita.h>

#include "spotify/session.h"
#include "album_grid.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_LIBRARY_PAGE (spotifygtk_library_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkLibraryPage, spotifygtk_library_page,
                      SPOTIFYGTK, LIBRARY_PAGE, GtkBox)

void spotifygtk_library_page_resolve_playlist (SpotifyGtkLibraryPage *self,
  const gchar *uri, const gchar *name, const gchar *cover);

SpotifyGtkLibraryPage *spotifygtk_library_page_new (void);
/* Home's device shortcut selects the same Local Files view as its toggle. */
void spotifygtk_library_page_show_local (SpotifyGtkLibraryPage *self);

/* Set the READY session, then fill Albums lazily on first visit. */
void spotifygtk_library_page_set_session (SpotifyGtkLibraryPage *self,
                                          SpotifyNativeSession  *session);
void spotifygtk_library_page_refresh (SpotifyGtkLibraryPage *self);

/* The window supplies the rootlist/device-playlist snapshot. The page copies
 * it, so the rootlist callback may release its temporary entries immediately. */
void spotifygtk_library_page_set_playlists (SpotifyGtkLibraryPage *self,
                                             const SpotifyGtkCardSpec *cards,
                                             guint n_cards);

/* Reflect one confirmed album collection write without rebuilding the whole
 * saved-album catalogue. Saving resolves only that album's metadata; removing
 * drops it from both the backing model and the visible filtered grid. */
void spotifygtk_library_page_set_album_saved (SpotifyGtkLibraryPage *self,
                                              const gchar           *uri,
                                              gboolean               saved);

/* The albums grid, so the window can wire "album-activated" to context nav. */
SpotifyGtkAlbumGrid *spotifygtk_library_page_get_album_grid (SpotifyGtkLibraryPage *self);
SpotifyGtkAlbumGrid *spotifygtk_library_page_get_alt_album_grid (SpotifyGtkLibraryPage *self);
SpotifyGtkAlbumGrid *spotifygtk_library_page_get_artist_grid (SpotifyGtkLibraryPage *self);

/* Copies the window's authoritative followed-artist URI set. Artist metadata
 * is resolved lazily when its card first becomes visible. */
void spotifygtk_library_page_set_followed_artists (SpotifyGtkLibraryPage *self,
                                                   GHashTable            *uris);

/* Drop textures held by hidden Library widgets, or restore only the currently
 * visible grid. The disk cache remains untouched. */
void spotifygtk_library_page_set_covers_loaded (SpotifyGtkLibraryPage *self,
                                                gboolean               loaded);

/* Signals:
 * - loading-changed (gboolean loading)
 * - new-playlist-requested ()
 */

G_END_DECLS

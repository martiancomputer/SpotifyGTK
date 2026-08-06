/*
 * album_grid.h — A grid or shelf of album cards derived from a track list.
 *
 * See album_grid.c for why the albums come from the tracks themselves rather
 * than a dedicated endpoint (there isn't one on the native stack).
 */

#pragma once

#include <gtk/gtk.h>

#include "spotify/session.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_ALBUM_GRID (spotifygtk_album_grid_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkAlbumGrid, spotifygtk_album_grid,
                      SPOTIFYGTK, ALBUM_GRID, GtkBox)

/* A horizontal, scrolling shelf (Home/Search) or a wrapping grid (Library). */
SpotifyGtkAlbumGrid *spotifygtk_album_grid_new_shelf (void);
SpotifyGtkAlbumGrid *spotifygtk_album_grid_new_grid (void);

/*
 * Replace the cards with the distinct albums present in `tracks` (a GPtrArray
 * of SpotifyNativeTrack*), in first-seen order, up to `max_albums`. The array
 * is only read, not kept. Returns how many cards were shown.
 */
guint spotifygtk_album_grid_set_from_tracks (SpotifyGtkAlbumGrid *self,
                                             GPtrArray           *tracks,
                                             guint                max_albums);

/* Add one card directly. For things that are not albums -- playlists have no
 * album to group by and carry no cover of their own. `cover_id` may be NULL
 * and supplied later. */
void spotifygtk_album_grid_add_card (SpotifyGtkAlbumGrid *self, const gchar *uri,
                                     const gchar *title, const gchar *subtitle,
                                     const gchar *cover_id);

/* Replace a card's title, matched by URI. For titles that arrive after the
 * card does. */
void spotifygtk_album_grid_set_card_title (SpotifyGtkAlbumGrid *self,
                                           const gchar *uri, const gchar *title);

/* Attach a cover to an already-added card, matched by URI. */
void spotifygtk_album_grid_set_card_cover (SpotifyGtkAlbumGrid *self,
                                           const gchar *uri, const gchar *cover_id);

void spotifygtk_album_grid_clear (SpotifyGtkAlbumGrid *self);

/* Inset the cards while leaving the scrollbar flush with the widget edge.
 * Use this rather than a margin on the widget itself, which would push the
 * scrollbar inward too and leave a gutter beside it. */
void spotifygtk_album_grid_set_content_margins (SpotifyGtkAlbumGrid *self,
                                                int start, int end);

/* Scrolling adjustment of the grid, for pages that react to scroll position. */
GtkAdjustment *spotifygtk_album_grid_get_vadjustment (SpotifyGtkAlbumGrid *self);

/* Signal: album-activated (const gchar *uri, const gchar *name) */

G_END_DECLS

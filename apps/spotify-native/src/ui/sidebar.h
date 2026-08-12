/*
 * sidebar.h — Navigation sidebar matching the mockup.
 *
 * 270px wide, dark theme, with:
 * - Main navigation (Home, Search, Liked Songs, Library, Downloads)
 * - Divider
 * - Pinned section (playlists/albums)
 */

#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_SIDEBAR (spotifygtk_sidebar_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkSidebar, spotifygtk_sidebar,
                      SPOTIFYGTK, SIDEBAR, GtkBox)

SpotifyGtkSidebar *spotifygtk_sidebar_new (void);

/* Signals:
 * - page-activated    (const gchar *page_id)
 * - pinned-activated  (const gchar *pinned_id)
 * - pin-requested     ()   the bottom action; the window knows what is on screen
 * - collapse-toggled  ()
 */

/* Pin management */
void spotifygtk_sidebar_add_pinned (SpotifyGtkSidebar *self,
                                    const gchar *id,
                                    const gchar *name,
                                    const gchar *type,
                                    const gchar *cover_id);
void spotifygtk_sidebar_clear_pinned (SpotifyGtkSidebar *self);

G_END_DECLS

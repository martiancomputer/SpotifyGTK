/*
 * album_page.h — Album detail view for SpotifyGTK Native.
 *
 * Shows:
 * - Album art (large)
 * - Album name, artist, year, track count
 * - Play/Shuffle buttons
 * - Track listing with duration
 * - "Save to Library" toggle
 */

#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_ALBUM_PAGE (spotifygtk_album_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkAlbumPage, spotifygtk_album_page,
                      SPOTIFYGTK, ALBUM_PAGE, AdwBin)

SpotifyGtkAlbumPage *spotifygtk_album_page_new (void);

/* Populate album data */
void spotifygtk_album_page_set_album (SpotifyGtkAlbumPage *self,
                                      JsonObject *album_data);
void spotifygtk_album_page_set_loading (SpotifyGtkAlbumPage *self,
                                        gboolean loading);
void spotifygtk_album_page_set_saved (SpotifyGtkAlbumPage *self,
                                      gboolean is_saved);

/* Signals:
 * - play-track (const gchar *track_uri)
 * - play-album (const gchar *album_uri)
 * - shuffle-album (const gchar *album_uri)
 * - toggle-saved (const gchar *album_id, gboolean save)
 * - artist-activated (const gchar *artist_id)
 */

G_END_DECLS

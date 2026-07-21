/*
 * artist_page.h — Artist detail view for SpotifyGTK Native.
 *
 * Shows:
 * - Artist image and name
 * - Follow/Unfollow button
 * - Popular tracks (top 10)
 * - Discography (albums, singles, appears on)
 * - Related artists
 */

#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_ARTIST_PAGE (spotifygtk_artist_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkArtistPage, spotifygtk_artist_page,
                      SPOTIFYGTK, ARTIST_PAGE, AdwBin)

SpotifyGtkArtistPage *spotifygtk_artist_page_new (void);

/* Populate artist data */
void spotifygtk_artist_page_set_artist (SpotifyGtkArtistPage *self,
                                        JsonObject *artist_data);
void spotifygtk_artist_page_set_top_tracks (SpotifyGtkArtistPage *self,
                                            JsonArray *tracks);
void spotifygtk_artist_page_set_albums (SpotifyGtkArtistPage *self,
                                        JsonArray *albums);
void spotifygtk_artist_page_set_related_artists (SpotifyGtkArtistPage *self,
                                                 JsonArray *artists);
void spotifygtk_artist_page_set_following (SpotifyGtkArtistPage *self,
                                           gboolean is_following);
void spotifygtk_artist_page_set_loading (SpotifyGtkArtistPage *self,
                                         gboolean loading);

/* Signals:
 * - play-track (const gchar *track_uri)
 * - play-artist-top (const gchar *artist_id)
 * - toggle-follow (const gchar *artist_id, gboolean follow)
 * - album-activated (const gchar *album_id)
 * - related-artist-activated (const gchar *artist_id)
 */

G_END_DECLS

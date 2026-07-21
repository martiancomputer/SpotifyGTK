/*
 * playlist_page.h — Playlist detail view for SpotifyGTK Native.
 *
 * Shows:
 * - Playlist cover art
 * - Playlist name, description, owner, track count
 * - Play/Shuffle buttons
 * - Track listing
 * - Search within playlist
 */

#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_PLAYLIST_PAGE (spotifygtk_playlist_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkPlaylistPage, spotifygtk_playlist_page,
                      SPOTIFYGTK, PLAYLIST_PAGE, AdwBin)

SpotifyGtkPlaylistPage *spotifygtk_playlist_page_new (void);

/* Populate playlist data */
void spotifygtk_playlist_page_set_playlist (SpotifyGtkPlaylistPage *self,
                                            JsonObject *playlist_data);
void spotifygtk_playlist_page_set_tracks (SpotifyGtkPlaylistPage *self,
                                          JsonArray *tracks);
void spotifygtk_playlist_page_append_tracks (SpotifyGtkPlaylistPage *self,
                                             JsonArray *tracks);
void spotifygtk_playlist_page_set_loading (SpotifyGtkPlaylistPage *self,
                                           gboolean loading);

/* Signals:
 * - play-track (const gchar *track_uri)
 * - play-playlist (const gchar *playlist_uri)
 * - shuffle-playlist (const gchar *playlist_uri)
 */

G_END_DECLS

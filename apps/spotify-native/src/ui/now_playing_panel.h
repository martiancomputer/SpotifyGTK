/*
 * now_playing_panel.h — Right-side Now Playing panel matching the mockup.
 *
 * 300px wide, dark theme (#202225), with:
 * - "Now Playing" header with collapse button
 * - Album art (220x220)
 * - Track title, artist, album
 * - Queue list below
 */

#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_NOW_PLAYING_PANEL (spotifygtk_now_playing_panel_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkNowPlayingPanel, spotifygtk_now_playing_panel,
                      SPOTIFYGTK, NOW_PLAYING_PANEL, GtkBox)

SpotifyGtkNowPlayingPanel *spotifygtk_now_playing_panel_new (void);

/* Update displayed information */
void spotifygtk_now_playing_panel_set_track (SpotifyGtkNowPlayingPanel *self,
                                             const gchar *track_name,
                                             const gchar *artist,
                                             const gchar *album);
void spotifygtk_now_playing_panel_set_album_art (SpotifyGtkNowPlayingPanel *self,
                                                 const gchar *image_path);
void spotifygtk_now_playing_panel_set_playing (SpotifyGtkNowPlayingPanel *self,
                                               gboolean is_playing);
void spotifygtk_now_playing_panel_set_progress (SpotifyGtkNowPlayingPanel *self,
                                                gint64 position_ms,
                                                gint64 duration_ms);

/* Queue */
void spotifygtk_now_playing_panel_set_queue (SpotifyGtkNowPlayingPanel *self,
                                             JsonArray *tracks);

void spotifygtk_now_playing_panel_set_cover (SpotifyGtkNowPlayingPanel *self, const gchar *cover_id);

G_END_DECLS

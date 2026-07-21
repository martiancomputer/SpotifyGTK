/*
 * now_playing_bar.h — Persistent playback bar for SpotifyGTK Native.
 *
 * A Spotify-style bottom bar showing:
 * - Album art thumbnail
 * - Track and artist name
 * - Playback controls (prev/play-pause/next)
 * - Progress bar with time display
 * - Volume control
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_NOW_PLAYING_BAR (spotifygtk_now_playing_bar_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkNowPlayingBar, spotifygtk_now_playing_bar,
                      SPOTIFYGTK, NOW_PLAYING_BAR, GtkWidget)

SpotifyGtkNowPlayingBar *spotifygtk_now_playing_bar_new (void);

/* Update displayed information */
void spotifygtk_now_playing_bar_set_track (SpotifyGtkNowPlayingBar *self,
                                           const gchar *track_name,
                                           const gchar *artist_name);
void spotifygtk_now_playing_bar_set_album_art (SpotifyGtkNowPlayingBar *self,
                                               const gchar *image_path);
void spotifygtk_now_playing_bar_set_playing (SpotifyGtkNowPlayingBar *self,
                                             gboolean is_playing);
void spotifygtk_now_playing_bar_set_progress (SpotifyGtkNowPlayingBar *self,
                                              gint64 position_ms,
                                              gint64 duration_ms);
void spotifygtk_now_playing_bar_set_volume (SpotifyGtkNowPlayingBar *self,
                                            gint volume_percent);
gint spotifygtk_now_playing_bar_get_volume (SpotifyGtkNowPlayingBar *self);

/* Signals:
 * - play-clicked
 * - pause-clicked
 * - next-clicked
 * - previous-clicked
 * - seek (gint64 position_ms)
 * - volume-changed (gint volume_percent)
 */

G_END_DECLS

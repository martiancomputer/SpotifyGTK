/*
 * playback_bar.h — Bottom playback bar matching the mockup.
 *
 * 80px tall, dark theme (#171717), with:
 * - Album art thumbnail (56x48)
 * - Track title
 * - Playback controls: prev, play/pause, next, shuffle, queue indicator
 * - Progress bar
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_PLAYBACK_BAR (spotifygtk_playback_bar_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkPlaybackBar, spotifygtk_playback_bar,
                      SPOTIFYGTK, PLAYBACK_BAR, GtkBox)

SpotifyGtkPlaybackBar *spotifygtk_playback_bar_new (void);

/* Update displayed information */
void spotifygtk_playback_bar_set_track (SpotifyGtkPlaybackBar *self,
                                        const gchar *track_name,
                                        const gchar *artist);
void spotifygtk_playback_bar_set_playing (SpotifyGtkPlaybackBar *self,
                                          gboolean is_playing);
void spotifygtk_playback_bar_set_progress (SpotifyGtkPlaybackBar *self,
                                           gint64 position_ms,
                                           gint64 duration_ms);
void spotifygtk_playback_bar_set_queue_count (SpotifyGtkPlaybackBar *self,
                                              gint count);

/* Signals:
 * - play-clicked
 * - pause-clicked
 * - next-clicked
 * - prev-clicked
 * - seek (gint64 position_ms)
 */

G_END_DECLS

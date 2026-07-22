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
void spotifygtk_playback_bar_set_volume       (SpotifyGtkPlaybackBar *self,
                                               gint percent);
void spotifygtk_playback_bar_set_liked        (SpotifyGtkPlaybackBar *self,
                                               gboolean liked);

void spotifygtk_playback_bar_set_cover (SpotifyGtkPlaybackBar *self, const gchar *cover_id);

/* Enable/disable the Previous and Next transport buttons. The window drives
 * this from its play-context and queue: Previous when an earlier track
 * exists, Next when a later track or a queued track exists. */
void spotifygtk_playback_bar_set_skip_sensitive (SpotifyGtkPlaybackBar *self,
                                                 gboolean can_prev,
                                                 gboolean can_next);

/* Signals:
 * - play-clicked
 * - pause-clicked
 * - next-clicked
 * - prev-clicked
 * - seek (gint64 position_ms)
 * - volume-changed (gint percent)
 * - like-toggled    (gboolean liked)
 * - shuffle-toggled (gboolean enabled)
 * - repeat-toggled  (gboolean enabled)
 * - queue-clicked
 */

G_END_DECLS

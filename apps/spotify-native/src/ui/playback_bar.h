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

/*
 * Repeat cycles rather than toggles: off, then the whole context, then the one
 * track. Two states cannot express "keep playing this track", which is the
 * half of repeat people reach for deliberately.
 */
typedef enum {
  SPOTIFYGTK_REPEAT_OFF = 0,
  SPOTIFYGTK_REPEAT_ALL,
  SPOTIFYGTK_REPEAT_ONE,
} SpotifyGtkRepeatMode;

/* Spotify's shuffle control has three states. Smart Shuffle still shuffles
 * the context, but asks Connect for a server-enhanced sequence containing
 * recommendations, so it cannot be represented by a boolean toggle. */
typedef enum {
  SPOTIFYGTK_SHUFFLE_OFF = 0,
  SPOTIFYGTK_SHUFFLE_NORMAL,
  SPOTIFYGTK_SHUFFLE_SMART,
} SpotifyGtkShuffleMode;

#define SPOTIFYGTK_TYPE_PLAYBACK_BAR (spotifygtk_playback_bar_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkPlaybackBar, spotifygtk_playback_bar,
                      SPOTIFYGTK, PLAYBACK_BAR, GtkBox)

SpotifyGtkPlaybackBar *spotifygtk_playback_bar_new (void);

/* Set both toggles without emitting, for restoring persisted state. */
void spotifygtk_playback_bar_set_modes (SpotifyGtkPlaybackBar *self,
                                        SpotifyGtkShuffleMode shuffle,
                                        SpotifyGtkRepeatMode repeat);

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
 * - shuffle-mode-changed (SpotifyGtkShuffleMode mode)
 * - repeat-changed  (SpotifyGtkRepeatMode mode)
 * - queue-clicked
 */

G_END_DECLS

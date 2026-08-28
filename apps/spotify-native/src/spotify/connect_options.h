#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Return 0=off, 1=ordinary shuffle, 2=Smart Shuffle. */
guint spotifygtk_connect_player_shuffle_mode  (const guint8 *options,
                                               gsize len,
                                               guint fallback);
guint spotifygtk_connect_command_shuffle_mode (const guint8 *options,
                                               gsize len,
                                               guint fallback);

/* Advance a reported playback position to `now_ms` without letting wall-clock
 * adjustments or integer overflow produce a backward/invalid position. All
 * clock values are monotonic milliseconds. */
gint64 spotifygtk_connect_live_position (gint64 position_ms,
                                         gint64 observed_at_ms,
                                         gint64 now_ms,
                                         gboolean playing);

G_END_DECLS

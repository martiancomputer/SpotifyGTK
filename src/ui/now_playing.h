#pragma once
#include <adwaita.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_NOW_PLAYING_BAR (spotifygtk_now_playing_bar_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyNowPlayingBar, spotifygtk_now_playing_bar,
                      SPOTIFYGTK, NOW_PLAYING_BAR, GtkBox)

SpotifyNowPlayingBar *spotifygtk_now_playing_bar_new (void);
void spotifygtk_now_playing_bar_update (SpotifyNowPlayingBar *self,
                                       const gchar *track, const gchar *artist,
                                       gboolean is_playing, gint64 position_ms, gint64 duration_ms);

G_END_DECLS

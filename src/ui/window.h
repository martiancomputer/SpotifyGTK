#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_WINDOW (spotifygtk_window_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyWindow, spotifygtk_window, SPOTIFYGTK, WINDOW, AdwApplicationWindow)

SpotifyWindow *spotifygtk_window_new               (GtkApplication *app);
void           spotifygtk_window_set_authenticated (SpotifyWindow  *self, gboolean authenticated);

G_END_DECLS

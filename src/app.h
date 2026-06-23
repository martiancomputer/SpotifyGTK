#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_APP (spotifygtk_app_get_type ())

G_DECLARE_FINAL_TYPE (SpotifyGtkApp, spotifygtk_app, SPOTIFYGTK, APP, AdwApplication)

SpotifyGtkApp *spotifygtk_app_new (void);

G_END_DECLS

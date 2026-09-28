#pragma once

#include "spotify/session.h"

G_BEGIN_DECLS

typedef struct _SpotifyGtkLocalFavorites SpotifyGtkLocalFavorites;

/* Persistent device data, deliberately independent of Spotify's collection
 * and cache. Returned tracks are borrowed until the next mutation. */
SpotifyGtkLocalFavorites *spotifygtk_local_favorites_new (void);
void spotifygtk_local_favorites_free (SpotifyGtkLocalFavorites *self);
gboolean spotifygtk_local_favorites_contains (SpotifyGtkLocalFavorites *self,
                                              const gchar *uri);
gboolean spotifygtk_local_favorites_set (SpotifyGtkLocalFavorites *self,
                                         const SpotifyNativeTrack *track,
                                         gboolean liked);
void spotifygtk_local_favorites_foreach (SpotifyGtkLocalFavorites *self,
                                         GHFunc callback, gpointer user_data);

G_END_DECLS

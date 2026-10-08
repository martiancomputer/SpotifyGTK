#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_PLAYLIST_CREATE_DIALOG (spotifygtk_playlist_create_dialog_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkPlaylistCreateDialog, spotifygtk_playlist_create_dialog,
                      SPOTIFYGTK, PLAYLIST_CREATE_DIALOG, AdwAlertDialog)

/* Emits create-requested (boolean spotify, string name) exactly once per
 * attempt. The caller completes the attempt, or reports an error for retry. */
SpotifyGtkPlaylistCreateDialog *spotifygtk_playlist_create_dialog_new (gboolean spotify_available);
void spotifygtk_playlist_create_dialog_complete (SpotifyGtkPlaylistCreateDialog *self,
                                                 const gchar *error);

G_END_DECLS

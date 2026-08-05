/*
 * track_row.h — Reusable track list row widget.
 *
 * A single row for displaying a track in a list, with:
 * - Track number or album art
 * - Track name and explicit badge
 * - Artist name
 * - Album name (optional)
 * - Duration
 * - Play/Add-to-queue actions on hover
 */

#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

#include "spotify/session.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_TRACK_ROW (spotifygtk_track_row_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkTrackRow, spotifygtk_track_row,
                      SPOTIFYGTK, TRACK_ROW, GtkBox)

SpotifyGtkTrackRow *spotifygtk_track_row_new (void);

/* Set track data from a Web API JSON object (spotify-connect's shape). */
void spotifygtk_track_row_set_track (SpotifyGtkTrackRow *self,
                                     JsonObject *track_data,
                                     gint track_number);

/* Set track data from the native session (spclient context + extended
 * metadata). This is the path spotify-native uses; the JSON variant above
 * remains for the Web API shape. */
void spotifygtk_track_row_set_native_track (SpotifyGtkTrackRow       *self,
                                            const SpotifyNativeTrack *track,
                                            gint                      track_number);
void spotifygtk_track_row_set_show_album (SpotifyGtkTrackRow *self,
                                          gboolean show);
void spotifygtk_track_row_set_show_artists (SpotifyGtkTrackRow *self,
                                            gboolean show);
/* Re-request this row's cover if it was skipped because loading was deferred
 * during a scroll. No-op when the art is already showing. */
/* Liked state: filled accent heart when liked, outline when not. */
void spotifygtk_track_row_set_liked (SpotifyGtkTrackRow *self, gboolean liked);

/* Hide the like control (Liked Songs page) while keeping its width. */
void spotifygtk_track_row_set_like_visible (SpotifyGtkTrackRow *self, gboolean visible);

void spotifygtk_track_row_retry_cover (SpotifyGtkTrackRow *self);

void spotifygtk_track_row_set_playing (SpotifyGtkTrackRow *self,
                                       gboolean is_playing,
                                       gboolean is_paused);

/* Get track info */
const gchar *spotifygtk_track_row_get_uri (SpotifyGtkTrackRow *self);
const gchar *spotifygtk_track_row_get_id (SpotifyGtkTrackRow *self);

/* Signals:
 * - play-clicked
 * - queue-clicked
 * - artist-activated (const gchar *artist_id)
 * - album-activated (const gchar *album_id)
 */

G_END_DECLS

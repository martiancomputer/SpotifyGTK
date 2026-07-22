/*
 * track_list.h — Reusable scrolling list of TrackRows.
 *
 * Every page that shows tracks (liked songs, playlist contents, search
 * results) needs the same three things: a scroller, rows built from a
 * JsonArray, and a status line for empty/error/loading states. This is that,
 * so the pages themselves only deal with which endpoint to call.
 */

#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

#include "spotify/session.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_TRACK_LIST (spotifygtk_track_list_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkTrackList, spotifygtk_track_list,
                      SPOTIFYGTK, TRACK_LIST, GtkBox)

SpotifyGtkTrackList *spotifygtk_track_list_new (void);


/*
 * Populate from the native session. `tracks` is a GPtrArray of
 * SpotifyNativeTrack* as returned by
 * spotifygtk_native_session_load_tracks_finish(); the list takes its own
 * copies, so the caller may unref it immediately.
 *
 * Emits "track-activated" with a SpotifyNativeTrack* (not a JsonObject)
 * when a row from this path is activated — see the signal note below.
 */
void spotifygtk_track_list_set_native_tracks (SpotifyGtkTrackList *self,
                                              GPtrArray           *tracks);

/* Show a message instead of rows (loading, empty, error). */
void spotifygtk_track_list_set_status (SpotifyGtkTrackList *self, const gchar *message);

void spotifygtk_track_list_clear (SpotifyGtkTrackList *self);

/* Number rows 1..n rather than showing per-row art. */
void spotifygtk_track_list_set_numbered (SpotifyGtkTrackList *self, gboolean numbered);

/* Mark whichever row matches `uri` as the current track. Pass NULL to clear.
 * `playing` false with a non-NULL uri means paused: the row keeps its
 * indicator but stops animating. */
void spotifygtk_track_list_set_playing_uri (SpotifyGtkTrackList *self,
                                            const gchar *uri,
                                            gboolean     playing);

/*
 * Snapshot the list's current tracks, in display order, as a fresh GPtrArray
 * of SpotifyNativeTrack* (deep copies, free func set). Used as the play
 * context so Next/Previous can walk the same ordering the user is looking at,
 * independent of the list being later cleared or refilled. Returns an empty
 * array (never NULL) for an empty list; unref when done.
 */
GPtrArray *spotifygtk_track_list_snapshot (SpotifyGtkTrackList *self);

/* Signals:
 * - track-activated (gpointer track)
 *
 *   The payload type follows whichever setter populated the list:
 *   JsonObject* after set_tracks(), SpotifyNativeTrack* after
 *   set_native_tracks(). A given list is only ever filled by one of them,
 *   so a page always knows which it is receiving.
 *
 * - add-to-queue (gpointer track)   — SpotifyNativeTrack*, right-click menu
 * - go-to-album  (gpointer track)   — SpotifyNativeTrack*, right-click menu
 * - go-to-artist (gpointer track)   — SpotifyNativeTrack*, right-click menu
 *
 *   For all three the payload is a SpotifyNativeTrack* that is only valid for
 *   the duration of the emission; a handler that needs to keep it must copy.
 */

G_END_DECLS

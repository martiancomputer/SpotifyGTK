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

/* Borrow the shared set of liked URIs. Not copied -- consulted on bind. */
void spotifygtk_track_list_set_liked_set (SpotifyGtkTrackList *self, GHashTable *set);

/* Repaint hearts for rows currently on screen, after the shared set changes. */
void spotifygtk_track_list_refresh_liked (SpotifyGtkTrackList *self);

/* Show or hide the per-row like control. Hidden on the Liked Songs page. */
void spotifygtk_track_list_set_show_like (SpotifyGtkTrackList *self, gboolean show);

/*
 * Splice one row in or out, leaving the rest of the list alone.
 *
 * For a membership change of exactly one row -- a like or an unlike. Handing
 * set_native_tracks() the new listing instead rebuilds every row and loses the
 * scroll position, which on a large library is a jump from wherever the user
 * was back to the top. Numbering follows automatically.
 *
 * The caller is responsible for the position matching its own model.
 */
void spotifygtk_track_list_insert_native_track (SpotifyGtkTrackList      *self,
                                                guint                     position,
                                                const SpotifyNativeTrack *track);
void spotifygtk_track_list_remove_position (SpotifyGtkTrackList *self, guint position);

/* Release the artwork of every bound row, for a page that is no longer
 * visible. Ids are kept, so the art returns when the page does. */
void spotifygtk_track_list_release_covers (SpotifyGtkTrackList *self);

/* Re-request artwork for every bound row, after release_covers. */
void spotifygtk_track_list_reload_covers (SpotifyGtkTrackList *self);

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
/* Size to content and let an outer scroller do the scrolling, for a list that
 * is one section of a longer page rather than the page itself. */
/* Right edge of the duration column in `relative_to` coordinates, for a header
 * that wants to align with it. FALSE until a row is laid out. */
gboolean spotifygtk_track_list_duration_edge (SpotifyGtkTrackList *self,
                                              GtkWidget *relative_to,
                                              gdouble *out_x);

void spotifygtk_track_list_set_inline (SpotifyGtkTrackList *self, gboolean inlined);

void spotifygtk_track_list_set_numbered (SpotifyGtkTrackList *self, gboolean numbered);

/*
 * Reserve `px` of empty space above the first row, inside the scrollable area
 * so it scrolls away with the content. The search page uses this to float a
 * translucent header over the list: rows start below the header but slide up
 * underneath it as you scroll.
 */
void spotifygtk_track_list_set_top_inset (SpotifyGtkTrackList *self, gint px);

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
 * - add-to-liked (gpointer track)   — SpotifyNativeTrack*, right-click menu
 * - remove-from-liked (gpointer track) — the same entry when already saved
 * - add-to-playlist (gpointer track)  — opens the playlist chooser
 * - add-to-queue (gpointer track)   — SpotifyNativeTrack*, right-click menu
 * - go-to-album  (gpointer track)   — SpotifyNativeTrack*, right-click menu
 * - go-to-artist (gpointer track)   — SpotifyNativeTrack*, right-click menu
 *
 *   For all three the payload is a SpotifyNativeTrack* that is only valid for
 *   the duration of the emission; a handler that needs to keep it must copy.
 */

G_END_DECLS

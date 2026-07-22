/*
 * track_item.h — GObject wrapper for one track in a list model.
 *
 * GtkListView needs a GListModel of GObjects, not raw structs, so this boxes
 * the fields a row shows. It also carries the playing/paused state, with a
 * "changed" signal, so that marking a different row as current updates only
 * that row rather than rebuilding the list — which is the whole point of the
 * virtualised list.
 */

#pragma once

#include <glib-object.h>
#include "spotify/session.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_TRACK_ITEM (spotifygtk_track_item_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkTrackItem, spotifygtk_track_item,
                      SPOTIFYGTK, TRACK_ITEM, GObject)

/* Takes its own copy of `track`; the caller keeps ownership of theirs. */
SpotifyGtkTrackItem *spotifygtk_track_item_new (const SpotifyNativeTrack *track,
                                                guint number);

const SpotifyNativeTrack *spotifygtk_track_item_get_track  (SpotifyGtkTrackItem *self);
guint                     spotifygtk_track_item_get_number (SpotifyGtkTrackItem *self);
const gchar              *spotifygtk_track_item_get_uri    (SpotifyGtkTrackItem *self);

gboolean spotifygtk_track_item_get_playing (SpotifyGtkTrackItem *self);
gboolean spotifygtk_track_item_get_paused  (SpotifyGtkTrackItem *self);
void     spotifygtk_track_item_set_playing (SpotifyGtkTrackItem *self,
                                            gboolean playing, gboolean paused);

/* Signal: changed () — emitted when playing/paused changes. */

G_END_DECLS

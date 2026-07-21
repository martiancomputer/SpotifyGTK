/*
 * queue_page.h — Playback queue view for SpotifyGTK Native.
 *
 * Shows:
 * - Now Playing (current track)
 * - Next Up (queued tracks)
 * - Queue management (clear, reorder)
 */

#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_QUEUE_PAGE (spotifygtk_queue_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkQueuePage, spotifygtk_queue_page,
                      SPOTIFYGTK, QUEUE_PAGE, AdwBin)

SpotifyGtkQueuePage *spotifygtk_queue_page_new (void);

/* Populate queue */
void spotifygtk_queue_page_set_now_playing (SpotifyGtkQueuePage *self,
                                            JsonObject *track);
void spotifygtk_queue_page_set_queue (SpotifyGtkQueuePage *self,
                                      JsonArray *tracks);
void spotifygtk_queue_page_clear (SpotifyGtkQueuePage *self);

/* Signals:
 * - play-track (gint index)
 * - remove-track (gint index)
 * - clear-queue
 */

G_END_DECLS

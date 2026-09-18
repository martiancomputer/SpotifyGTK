#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Return 0=off, 1=ordinary shuffle, 2=Smart Shuffle. */
guint spotifygtk_connect_player_shuffle_mode  (const guint8 *options,
                                               gsize len,
                                               guint fallback);
guint spotifygtk_connect_command_shuffle_mode (const guint8 *options,
                                               gsize len,
                                               guint fallback);

/* Advance a reported playback position to `now_ms` without letting wall-clock
 * adjustments or integer overflow produce a backward/invalid position. All
 * clock values are monotonic milliseconds. */
gint64 spotifygtk_connect_live_position (gint64 position_ms,
                                         gint64 observed_at_ms,
                                         gint64 now_ms,
                                         gboolean playing);
/* Dealer pushes and HTTP responses can arrive out of order. Zero means the
 * sender omitted the timestamp; otherwise older snapshots must not win. */
gboolean spotifygtk_connect_accept_timestamp (guint64 *latest, guint64 received);

/* A same-track transfer is not a seek. Missing positions use -1; only a
 * genuinely different track adopts the transfer's zero/start position. */
gint64 spotifygtk_connect_transfer_position (gint64 local_ms, gint64 remote_ms,
                                              gboolean same_track);
gchar *spotifygtk_connect_track_uid (const gchar *uri, const gchar *provider, guint index);
/* Append ProvidedTrack to a PlayerState (field 7/19/20). Stable occurrence
 * UIDs preserve duplicates and providers distinguish user queue/context. */
void spotifygtk_connect_write_track (GByteArray *state, guint field,
  const gchar *uri, const gchar *provider, guint index);

typedef struct { gchar *uri, *uid, *provider; } SpotifyConnectTrack;
typedef struct {
  gchar *endpoint, *context_uri, *uri, *track_uid, *queue_revision;
  GPtrArray *next, *previous; /* SpotifyConnectTrack, ordered with duplicates */
  gint track_index, shuffle_mode, repeating_context, repeating_track;
  gint64 seek_ms;
  gboolean paused;
} SpotifyConnectCommand;
SpotifyConnectCommand *spotifygtk_connect_parse_command (const gchar *json, GError **error);
void spotifygtk_connect_command_free (SpotifyConnectCommand *command);
gchar *spotifygtk_connect_queue_revision (const gchar *current_uri,
  const gchar *const *next_uris, guint queued_count);

G_END_DECLS

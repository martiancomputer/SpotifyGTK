#pragma once

#include "spotify/session.h"
#include <string.h>

G_BEGIN_DECLS

typedef struct _SpotifyGtkDevicePlaylists SpotifyGtkDevicePlaylists;
typedef struct {
  gchar *uri;
  gchar *name;
  gchar *cover_id;
  gboolean overlay;
  gint64 added_at; /* seconds since epoch for device-created playlists */
} SpotifyGtkDevicePlaylistInfo;

SpotifyGtkDevicePlaylists *spotifygtk_device_playlists_new (void);
void spotifygtk_device_playlists_free (SpotifyGtkDevicePlaylists *self);

/* Fully local playlists can contain local and Spotify track snapshots. A
 * Spotify playlist overlay accepts local tracks only. All results are owned. */
gchar *spotifygtk_device_playlists_create (SpotifyGtkDevicePlaylists *self,
                                           const gchar *name);
gboolean spotifygtk_device_playlists_rename (SpotifyGtkDevicePlaylists *self,
                                              const gchar *uri,
                                              const gchar *name);
gboolean spotifygtk_device_playlists_delete (SpotifyGtkDevicePlaylists *self,
                                              const gchar *uri);
void spotifygtk_device_playlists_set_overlay_name (SpotifyGtkDevicePlaylists *self,
                                                    const gchar *uri,
                                                    const gchar *name);
gboolean spotifygtk_device_playlists_add (SpotifyGtkDevicePlaylists *self,
                                           const gchar *playlist_uri,
                                           const SpotifyNativeTrack *track);
gboolean spotifygtk_device_playlists_remove (SpotifyGtkDevicePlaylists *self,
                                              const gchar *playlist_uri,
                                              const gchar *track_uri,
                                              guint occurrence);
gboolean spotifygtk_device_playlists_remove_at (SpotifyGtkDevicePlaylists *self,
                                                 const gchar *playlist_uri,
                                                 guint index,
                                                 const gchar *expected_uri);
GPtrArray *spotifygtk_device_playlists_list (SpotifyGtkDevicePlaylists *self);
void spotifygtk_device_playlist_info_free (SpotifyGtkDevicePlaylistInfo *info);
/* A non-NULL server sequence pins newly added overlay entries once. Later
 * refreshes merge against URI/occurrence anchors. NULL means offline and
 * never establishes a new ordering anchor. */
GPtrArray *spotifygtk_device_playlists_tracks (SpotifyGtkDevicePlaylists *self,
                                               const gchar *playlist_uri,
                                               GPtrArray *server_tracks);

/* Only accepts the editor's bounded JPEG output. File I/O runs off the UI thread. */
void spotifygtk_device_playlists_set_artwork (SpotifyGtkDevicePlaylists *self,
  const gchar *uri, GBytes *jpeg, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data);
gboolean spotifygtk_device_playlists_set_artwork_finish (SpotifyGtkDevicePlaylists *self,
  GAsyncResult *result, GError **error);
/* Shared pure resolver: persisted ids can never name arbitrary local files. */
static inline gchar *
spotifygtk_device_playlists_dup_art_path (const gchar *cover_id)
{
  const gchar *prefix = "local:playlist-art:";
  if (!cover_id || !g_str_has_prefix (cover_id, prefix)) return NULL;
  const gchar *id = cover_id + strlen (prefix);
  if (strlen (id) != 64) return NULL;
  for (const gchar *p = id; *p; p++) if (!g_ascii_isxdigit (*p)) return NULL;
  return g_build_filename (g_get_user_data_dir (), "spotifygtk", "playlist-art", id, NULL);
}

G_END_DECLS

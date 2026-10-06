#pragma once

#include "spotify/session.h"

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
GPtrArray *spotifygtk_device_playlists_tracks (SpotifyGtkDevicePlaylists *self,
                                               const gchar *playlist_uri,
                                               GPtrArray *server_tracks);

G_END_DECLS

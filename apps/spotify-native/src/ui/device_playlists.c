#include "device_playlists.h"

#include <glib/gstdio.h>
#include <string.h>

#define MAX_PLAYLISTS 1000
#define MAX_PLAYLIST_ENTRIES 100000
#define MAX_PLAYLIST_DATA_BYTES (24 * 1024 * 1024)

typedef struct {
  gchar *uri;
  gchar *name; /* NULL for a Spotify playlist overlay */
  gchar *display_name; /* cached, device-only label for an overlay */
  GPtrArray *tracks; /* owned SpotifyNativeTrack, ordered */
} DevicePlaylist;

struct _SpotifyGtkDevicePlaylists {
  gatomicrefcount refs;
  GHashTable *lists; /* URI -> DevicePlaylist */
  GFile *file;
  guint entry_count;
  guint save_id;
  gboolean saving;
  gboolean dirty;
};

static gboolean save_playlists (gpointer data);

static void
playlist_free (gpointer data)
{
  DevicePlaylist *list = data;
  g_free (list->uri);
  g_free (list->name);
  g_free (list->display_name);
  g_ptr_array_unref (list->tracks);
  g_free (list);
}

static DevicePlaylist *
playlist_new (const gchar *uri, const gchar *name)
{
  DevicePlaylist *list = g_new0 (DevicePlaylist, 1);
  list->uri = g_strdup (uri);
  list->name = g_strdup (name);
  list->tracks = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_track_free);
  return list;
}

static gboolean
playlist_uri_valid (const gchar *uri)
{
  return uri && (g_str_has_prefix (uri, "local:playlist:") ||
                 g_str_has_prefix (uri, "spotify:playlist:")) &&
         strlen (uri) <= 128;
}

static gboolean
track_uri_valid (const gchar *uri)
{
  return uri && (g_str_has_prefix (uri, "local:track:") ||
                 g_str_has_prefix (uri, "spotify:track:")) &&
         strlen (uri) <= 128;
}

static gchar *
storage_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "spotifygtk",
                           "device-playlists.ini", NULL);
}

static void
store_unref (SpotifyGtkDevicePlaylists *self)
{
  if (!g_atomic_ref_count_dec (&self->refs)) return;
  g_hash_table_unref (self->lists);
  g_object_unref (self->file);
  g_free (self);
}

typedef struct {
  SpotifyGtkDevicePlaylists *store;
  gchar *contents;
} SaveOp;

static void
on_saved (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SaveOp *op = user_data;
  SpotifyGtkDevicePlaylists *self = op->store;
  g_autoptr(GError) error = NULL;
  if (!g_file_replace_contents_finish (G_FILE (source), result, NULL, &error))
    g_warning ("device playlists: save failed: %s", error->message);
  self->saving = FALSE;
  if (self->dirty && !self->save_id)
    save_playlists (self);
  g_free (op->contents);
  g_free (op);
  store_unref (self);
}

static gboolean
save_playlists (gpointer data)
{
  SpotifyGtkDevicePlaylists *self = data;
  self->save_id = 0;
  if (self->saving) {
    self->dirty = TRUE;
    return G_SOURCE_REMOVE;
  }
  self->dirty = FALSE;
  g_autoptr(GKeyFile) key = g_key_file_new ();
  GHashTableIter iter;
  gpointer value;
  guint index = 0;
  g_hash_table_iter_init (&iter, self->lists);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    DevicePlaylist *list = value;
    g_autofree gchar *group = g_strdup_printf ("playlist-%u", index);
    g_key_file_set_string (key, group, "uri", list->uri);
    if (list->name)
      g_key_file_set_string (key, group, "name", list->name);
    if (list->display_name)
      g_key_file_set_string (key, group, "display-name", list->display_name);
    g_key_file_set_integer (key, group, "count", list->tracks->len);
    for (guint j = 0; j < list->tracks->len; j++) {
      SpotifyNativeTrack *track = g_ptr_array_index (list->tracks, j);
      g_autofree gchar *entry = g_strdup_printf ("entry-%u-%u", index, j);
      g_key_file_set_string (key, entry, "uri", track->uri);
      g_key_file_set_string (key, entry, "title", track->name ? track->name : "");
      g_key_file_set_string (key, entry, "artists", track->artists ? track->artists : "");
      g_key_file_set_string (key, entry, "album", track->album ? track->album : "");
      g_key_file_set_string (key, entry, "cover", track->cover_id ? track->cover_id : "");
      g_key_file_set_int64 (key, entry, "duration", track->duration_ms);
    }
    index++;
  }
  gsize length = 0;
  gchar *contents = g_key_file_to_data (key, &length, NULL);
  if (!contents || length > MAX_PLAYLIST_DATA_BYTES) {
    g_warning ("device playlists: refusing oversized save");
    g_free (contents);
    return G_SOURCE_REMOVE;
  }
  g_autofree gchar *path = storage_path ();
  g_autofree gchar *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0) {
    g_free (contents);
    return G_SOURCE_REMOVE;
  }
  SaveOp *op = g_new0 (SaveOp, 1);
  op->store = self;
  op->contents = contents;
  g_atomic_ref_count_inc (&self->refs);
  self->saving = TRUE;
  g_file_replace_contents_async (self->file, contents, length, NULL, FALSE,
                                 G_FILE_CREATE_PRIVATE, NULL, on_saved, op);
  return G_SOURCE_REMOVE;
}

static void
schedule_save (SpotifyGtkDevicePlaylists *self)
{
  if (!self->save_id)
    self->save_id = g_timeout_add (250, save_playlists, self);
}

SpotifyGtkDevicePlaylists *
spotifygtk_device_playlists_new (void)
{
  SpotifyGtkDevicePlaylists *self = g_new0 (SpotifyGtkDevicePlaylists, 1);
  g_atomic_ref_count_init (&self->refs);
  self->lists = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                      playlist_free);
  g_autofree gchar *path = storage_path ();
  self->file = g_file_new_for_path (path);
  GStatBuf st;
  if (g_stat (path, &st) != 0 || st.st_size <= 0 ||
      st.st_size > MAX_PLAYLIST_DATA_BYTES)
    return self;
  g_autoptr(GKeyFile) key = g_key_file_new ();
  if (!g_key_file_load_from_file (key, path, G_KEY_FILE_NONE, NULL))
    return self;
  guint entries = 0;
  for (guint i = 0; i < MAX_PLAYLISTS; i++) {
    g_autofree gchar *group = g_strdup_printf ("playlist-%u", i);
    if (!g_key_file_has_group (key, group)) break;
    g_autofree gchar *uri = g_key_file_get_string (key, group, "uri", NULL);
    g_autofree gchar *name = g_key_file_get_string (key, group, "name", NULL);
    g_autofree gchar *display_name = g_key_file_get_string (
      key, group, "display-name", NULL);
    if (!playlist_uri_valid (uri) ||
        (g_str_has_prefix (uri, "local:") && (!name || !*name)) ||
        (name && strlen (name) > 256) ||
        (display_name && strlen (display_name) > 256))
      continue;
    gint count = g_key_file_get_integer (key, group, "count", NULL);
    if (count < 0 || (guint) count > MAX_PLAYLIST_ENTRIES - entries) continue;
    DevicePlaylist *list = playlist_new (uri, name);
    if (!name && display_name)
      list->display_name = g_strdup (display_name);
    for (gint j = 0; j < count; j++) {
      g_autofree gchar *entry = g_strdup_printf ("entry-%u-%d", i, j);
      g_autofree gchar *track_uri = g_key_file_get_string (key, entry, "uri", NULL);
      if (!track_uri_valid (track_uri) ||
          (name == NULL && !g_str_has_prefix (track_uri, "local:")))
        continue;
      SpotifyNativeTrack *track = g_new0 (SpotifyNativeTrack, 1);
      track->uri = g_strdup (track_uri);
      track->name = g_key_file_get_string (key, entry, "title", NULL);
      track->artists = g_key_file_get_string (key, entry, "artists", NULL);
      track->album = g_key_file_get_string (key, entry, "album", NULL);
      track->cover_id = g_key_file_get_string (key, entry, "cover", NULL);
      track->duration_ms = g_key_file_get_int64 (key, entry, "duration", NULL);
      if (!track->name || strlen (track->name) > 4096 ||
          !track->artists || strlen (track->artists) > 4096 ||
          !track->album || strlen (track->album) > 4096 ||
          !track->cover_id || strlen (track->cover_id) > 128) {
        spotifygtk_native_track_free (track);
        continue;
      }
      g_ptr_array_add (list->tracks, track);
      entries++;
    }
    g_hash_table_replace (self->lists, g_strdup (uri), list);
  }
  self->entry_count = entries;
  return self;
}

void
spotifygtk_device_playlists_free (SpotifyGtkDevicePlaylists *self)
{
  if (!self) return;
  if (self->save_id) {
    g_source_remove (self->save_id);
    save_playlists (self);
  }
  while (self->saving || self->dirty)
    g_main_context_iteration (NULL, TRUE);
  store_unref (self);
}

gchar *
spotifygtk_device_playlists_create (SpotifyGtkDevicePlaylists *self,
                                    const gchar *name)
{
  g_return_val_if_fail (self != NULL, NULL);
  if (!name || !*name || strlen (name) > 256 ||
      g_hash_table_size (self->lists) >= MAX_PLAYLISTS)
    return NULL;
  g_autofree gchar *uuid = g_uuid_string_random ();
  gchar *uri = g_strconcat ("local:playlist:", uuid, NULL);
  g_hash_table_replace (self->lists, g_strdup (uri), playlist_new (uri, name));
  schedule_save (self);
  return uri;
}

gboolean
spotifygtk_device_playlists_rename (SpotifyGtkDevicePlaylists *self,
                                     const gchar *uri, const gchar *name)
{
  DevicePlaylist *list = self && uri ? g_hash_table_lookup (self->lists, uri) : NULL;
  if (!list || !g_str_has_prefix (uri, "local:playlist:") ||
      !name || !*name || strlen (name) > 256)
    return FALSE;
  g_free (list->name);
  list->name = g_strdup (name);
  schedule_save (self);
  return TRUE;
}

gboolean
spotifygtk_device_playlists_delete (SpotifyGtkDevicePlaylists *self,
                                     const gchar *uri)
{
  DevicePlaylist *list = self && uri ? g_hash_table_lookup (self->lists, uri) : NULL;
  if (!list || !g_str_has_prefix (uri, "local:playlist:")) return FALSE;
  self->entry_count -= list->tracks->len;
  g_hash_table_remove (self->lists, uri);
  schedule_save (self);
  return TRUE;
}

void
spotifygtk_device_playlists_set_overlay_name (SpotifyGtkDevicePlaylists *self,
                                                const gchar *uri,
                                                const gchar *name)
{
  DevicePlaylist *list = self && uri ? g_hash_table_lookup (self->lists, uri) : NULL;
  if (!list || list->name || !name || !*name || strlen (name) > 256 ||
      !g_str_has_prefix (uri, "spotify:playlist:"))
    return;
  if (g_strcmp0 (list->display_name, name) == 0) return;
  g_free (list->display_name);
  list->display_name = g_strdup (name);
  schedule_save (self);
}

gboolean
spotifygtk_device_playlists_add (SpotifyGtkDevicePlaylists *self,
                                  const gchar *playlist_uri,
                                  const SpotifyNativeTrack *track)
{
  if (!self || !playlist_uri_valid (playlist_uri) || !track ||
      !track_uri_valid (track->uri)) return FALSE;
  if ((track->name && strlen (track->name) > 4096) ||
      (track->artists && strlen (track->artists) > 4096) ||
      (track->album && strlen (track->album) > 4096) ||
      (track->cover_id && strlen (track->cover_id) > 128) ||
      self->entry_count >= MAX_PLAYLIST_ENTRIES)
    return FALSE;
  if (g_str_has_prefix (playlist_uri, "spotify:playlist:") &&
      !g_str_has_prefix (track->uri, "local:track:"))
    return FALSE;
  DevicePlaylist *list = g_hash_table_lookup (self->lists, playlist_uri);
  if (!list) {
    if (!g_str_has_prefix (playlist_uri, "spotify:playlist:") ||
        g_hash_table_size (self->lists) >= MAX_PLAYLISTS)
      return FALSE;
    list = playlist_new (playlist_uri, NULL);
    g_hash_table_insert (self->lists, g_strdup (playlist_uri), list);
  }
  if (list->tracks->len >= MAX_PLAYLIST_ENTRIES)
    return FALSE;
  g_ptr_array_add (list->tracks, spotifygtk_native_track_copy (track));
  self->entry_count++;
  schedule_save (self);
  return TRUE;
}

gboolean
spotifygtk_device_playlists_remove (SpotifyGtkDevicePlaylists *self,
                                     const gchar *playlist_uri,
                                     const gchar *track_uri, guint occurrence)
{
  DevicePlaylist *list = self && playlist_uri
    ? g_hash_table_lookup (self->lists, playlist_uri) : NULL;
  if (!list || !track_uri) return FALSE;
  guint seen = 0;
  for (guint i = 0; i < list->tracks->len; i++) {
    SpotifyNativeTrack *track = g_ptr_array_index (list->tracks, i);
    if (g_strcmp0 (track->uri, track_uri) != 0) continue;
    if (seen++ != occurrence) continue;
    g_ptr_array_remove_index (list->tracks, i);
    self->entry_count--;
    schedule_save (self);
    return TRUE;
  }
  return FALSE;
}

gboolean
spotifygtk_device_playlists_remove_at (SpotifyGtkDevicePlaylists *self,
                                        const gchar *playlist_uri, guint index,
                                        const gchar *expected_uri)
{
  DevicePlaylist *list = self && playlist_uri
    ? g_hash_table_lookup (self->lists, playlist_uri) : NULL;
  if (!list || index >= list->tracks->len) return FALSE;
  const SpotifyNativeTrack *track = g_ptr_array_index (list->tracks, index);
  if (g_strcmp0 (track->uri, expected_uri) != 0) return FALSE;
  g_ptr_array_remove_index (list->tracks, index);
  self->entry_count--;
  schedule_save (self);
  return TRUE;
}

void
spotifygtk_device_playlist_info_free (SpotifyGtkDevicePlaylistInfo *info)
{
  if (!info) return;
  g_free (info->uri);
  g_free (info->name);
  g_free (info->cover_id);
  g_free (info);
}

GPtrArray *
spotifygtk_device_playlists_list (SpotifyGtkDevicePlaylists *self)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_device_playlist_info_free);
  if (!self) return out;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init (&iter, self->lists);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    DevicePlaylist *list = value;
    if (!list->name && !list->tracks->len) continue;
    SpotifyGtkDevicePlaylistInfo *info = g_new0 (SpotifyGtkDevicePlaylistInfo, 1);
    info->uri = g_strdup (list->uri);
    info->name = g_strdup (list->name ? list->name :
      list->display_name ? list->display_name : "Spotify playlist with device tracks");
    info->overlay = !list->name;
    if (list->tracks->len)
      info->cover_id = g_strdup (((SpotifyNativeTrack *)
        g_ptr_array_index (list->tracks, 0))->cover_id);
    g_ptr_array_add (out, info);
  }
  return out;
}

GPtrArray *
spotifygtk_device_playlists_tracks (SpotifyGtkDevicePlaylists *self,
                                    const gchar *playlist_uri,
                                    GPtrArray *server_tracks)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_track_free);
  DevicePlaylist *list = self && playlist_uri
    ? g_hash_table_lookup (self->lists, playlist_uri) : NULL;
  for (guint i = 0; server_tracks && i < server_tracks->len; i++)
    g_ptr_array_add (out, spotifygtk_native_track_copy (
      g_ptr_array_index (server_tracks, i)));
  if (list)
    for (guint i = 0; i < list->tracks->len; i++) {
      SpotifyNativeTrack *track = spotifygtk_native_track_copy (
        g_ptr_array_index (list->tracks, i));
      g_free (track->section_detail);
      track->section_detail = g_strdup ("On this device");
      g_ptr_array_add (out, track);
    }
  return out;
}

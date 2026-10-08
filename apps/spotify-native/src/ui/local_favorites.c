#include "local_favorites.h"

#include <glib/gstdio.h>
#include <string.h>

#define MAX_LOCAL_FAVORITES 20000
#define MAX_FAVORITES_BYTES (8 * 1024 * 1024)

struct _SpotifyGtkLocalFavorites {
  gatomicrefcount refs;
  GHashTable *tracks; /* URI -> owned SpotifyNativeTrack */
  GFile *file;
  guint save_id;
  gboolean saving;
  gboolean dirty;
};

static gboolean save_favorites (gpointer data);

static void
favorites_unref (SpotifyGtkLocalFavorites *self)
{
  if (!g_atomic_ref_count_dec (&self->refs))
    return;
  g_hash_table_unref (self->tracks);
  g_object_unref (self->file);
  g_free (self);
}

typedef struct {
  SpotifyGtkLocalFavorites *favorites;
  gchar *contents;
} SaveOp;

static gchar *
favorite_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "spotifygtk",
                           "local-favorites.ini", NULL);
}

static void
on_saved (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SaveOp *op = user_data;
  SpotifyGtkLocalFavorites *self = op->favorites;
  g_autoptr(GError) error = NULL;
  if (!g_file_replace_contents_finish (G_FILE (source), result, NULL, &error))
    g_warning ("local favorites: save failed: %s", error->message);
  self->saving = FALSE;
  if (self->dirty && !self->save_id)
    save_favorites (self);
  g_free (op->contents);
  g_free (op);
  favorites_unref (self);
}

static gboolean
save_favorites (gpointer data)
{
  SpotifyGtkLocalFavorites *self = data;
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
  g_hash_table_iter_init (&iter, self->tracks);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    const SpotifyNativeTrack *t = value;
    g_autofree gchar *group = g_strdup_printf ("track-%u", index++);
    g_key_file_set_string (key, group, "uri", t->uri);
    g_key_file_set_string (key, group, "title", t->name ? t->name : "");
    g_key_file_set_string (key, group, "artists", t->artists ? t->artists : "");
    g_key_file_set_string (key, group, "album", t->album ? t->album : "");
    g_key_file_set_string (key, group, "cover", t->cover_id ? t->cover_id : "");
    g_key_file_set_int64 (key, group, "duration", t->duration_ms);
    g_key_file_set_int64 (key, group, "liked-at", t->liked_at);
  }
  gsize length = 0;
  gchar *contents = g_key_file_to_data (key, &length, NULL);
  if (!contents || length > MAX_FAVORITES_BYTES) {
    g_warning ("local favorites: refusing oversized save");
    g_free (contents);
    return G_SOURCE_REMOVE;
  }
  g_autofree gchar *path = favorite_path ();
  g_autofree gchar *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0) {
    g_free (contents);
    return G_SOURCE_REMOVE;
  }
  SaveOp *op = g_new0 (SaveOp, 1);
  op->favorites = self;
  op->contents = contents;
  g_atomic_ref_count_inc (&self->refs);
  self->saving = TRUE;
  g_file_replace_contents_async (self->file, contents, length, NULL, FALSE,
                                 G_FILE_CREATE_PRIVATE, NULL, on_saved, op);
  return G_SOURCE_REMOVE;
}

SpotifyGtkLocalFavorites *
spotifygtk_local_favorites_new (void)
{
  SpotifyGtkLocalFavorites *self = g_new0 (SpotifyGtkLocalFavorites, 1);
  g_atomic_ref_count_init (&self->refs);
  self->tracks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                        (GDestroyNotify) spotifygtk_native_track_free);
  g_autofree gchar *path = favorite_path ();
  self->file = g_file_new_for_path (path);
  GStatBuf file_stat;
  if (g_stat (path, &file_stat) != 0 ||
      file_stat.st_size > MAX_FAVORITES_BYTES || file_stat.st_size < 0)
    return self;
  g_autoptr(GKeyFile) key = g_key_file_new ();
  if (!g_key_file_load_from_file (key, path, G_KEY_FILE_NONE, NULL))
    return self;
  gsize count = 0;
  g_auto(GStrv) groups = g_key_file_get_groups (key, &count);
  if (count > MAX_LOCAL_FAVORITES)
    return self;
  for (gsize i = 0; i < count; i++) {
    g_autofree gchar *uri = g_key_file_get_string (key, groups[i], "uri", NULL);
    if (!uri || !g_str_has_prefix (uri, "local:track:") || strlen (uri) > 128)
      continue;
    SpotifyNativeTrack *track = g_new0 (SpotifyNativeTrack, 1);
    track->uri = g_strdup (uri);
    track->name = g_key_file_get_string (key, groups[i], "title", NULL);
    track->artists = g_key_file_get_string (key, groups[i], "artists", NULL);
    track->album = g_key_file_get_string (key, groups[i], "album", NULL);
    track->cover_id = g_key_file_get_string (key, groups[i], "cover", NULL);
    track->duration_ms = g_key_file_get_int64 (key, groups[i], "duration", NULL);
    track->liked_at = MAX ((gint64) 0,
      g_key_file_get_int64 (key, groups[i], "liked-at", NULL));
    if (!track->name || strlen (track->name) > 4096 ||
        !track->artists || strlen (track->artists) > 4096 ||
        !track->album || strlen (track->album) > 4096 ||
        !track->cover_id || strlen (track->cover_id) > 128) {
      spotifygtk_native_track_free (track);
      continue;
    }
    g_hash_table_replace (self->tracks, g_strdup (uri), track);
  }
  return self;
}

void
spotifygtk_local_favorites_free (SpotifyGtkLocalFavorites *self)
{
  if (!self) return;
  if (self->save_id) {
    g_source_remove (self->save_id);
    save_favorites (self);
  }
  /* Window shutdown may stop the main loop immediately. Drain the one
   * in-flight atomic replacement (and any coalesced newer snapshot) before
   * dropping the store, otherwise a like made just before closing is lost. */
  while (self->saving || self->dirty)
    g_main_context_iteration (NULL, TRUE);
  favorites_unref (self);
}

gboolean
spotifygtk_local_favorites_contains (SpotifyGtkLocalFavorites *self,
                                      const gchar *uri)
{
  return self && uri && g_hash_table_contains (self->tracks, uri);
}

gboolean
spotifygtk_local_favorites_set (SpotifyGtkLocalFavorites *self,
                                const SpotifyNativeTrack *track, gboolean liked)
{
  if (!self || !track || !track->uri ||
      !g_str_has_prefix (track->uri, "local:track:"))
    return FALSE;
  if (strlen (track->uri) > 128 ||
      (track->name && strlen (track->name) > 4096) ||
      (track->artists && strlen (track->artists) > 4096) ||
      (track->album && strlen (track->album) > 4096) ||
      (track->cover_id && strlen (track->cover_id) > 128))
    return FALSE;
  if (liked) {
    if (!g_hash_table_contains (self->tracks, track->uri) &&
        g_hash_table_size (self->tracks) >= MAX_LOCAL_FAVORITES)
      return FALSE;
    const SpotifyNativeTrack *old = g_hash_table_lookup (self->tracks, track->uri);
    SpotifyNativeTrack *copy = spotifygtk_native_track_copy (track);
    /* Metadata refreshes are not new likes. Legacy entries remain undated:
     * inventing today's date would put old favorites above today's songs. */
    copy->liked_at = old ? old->liked_at : g_get_real_time () / G_USEC_PER_SEC;
    g_hash_table_replace (self->tracks, g_strdup (track->uri), copy);
  } else {
    g_hash_table_remove (self->tracks, track->uri);
  }
  if (!self->save_id)
    self->save_id = g_timeout_add (250, save_favorites, self);
  return TRUE;
}

void
spotifygtk_local_favorites_foreach (SpotifyGtkLocalFavorites *self,
                                    GHFunc callback, gpointer user_data)
{
  if (self && callback)
    g_hash_table_foreach (self->tracks, callback, user_data);
}

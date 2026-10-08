#include "catalog_cache.h"
#include <glib/gstdio.h>
#include <string.h>

#define ENTRY_LIMIT (4 * 1024 * 1024)
#define DISK_LIMIT (32 * 1024 * 1024)
#define FILE_LIMIT 256
static GMutex cache_lock;
static guint cache_epoch;
static gboolean cache_enabled = TRUE;

static gchar *cache_dir (void)
{
  return g_build_filename (g_get_user_cache_dir (), "spotifygtk", "catalog-v1", NULL);
}

static gchar *cache_path (const gchar *key)
{
  g_autofree gchar *dir = cache_dir ();
  g_autofree gchar *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, key, -1);
  return g_build_filename (dir, hash, NULL);
}

/* Only our hashed regular files are eligible for deletion. Never follow a
 * link or recursively remove a directory while pruning this bounded cache. */
typedef struct { gchar *path; gint64 time; glong nanoseconds; gsize size; } CacheFile;
static void cache_file_free (gpointer data)
{
  CacheFile *file = data;
  g_free (file->path);
  g_free (file);
}
static gint oldest_first (gconstpointer a, gconstpointer b)
{
  const CacheFile *fa = *(CacheFile *const *) a, *fb = *(CacheFile *const *) b;
  if (fa->time != fb->time)
    return (fa->time > fb->time) - (fa->time < fb->time);
  return (fa->nanoseconds > fb->nanoseconds) - (fa->nanoseconds < fb->nanoseconds);
}
static GPtrArray *cache_files (void)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (cache_file_free);
  g_autofree gchar *dir = cache_dir ();
  g_autoptr(GDir) scan = g_dir_open (dir, 0, NULL);
  const gchar *name;
  while (scan && (name = g_dir_read_name (scan))) {
    if (strlen (name) != 64) continue;
    gboolean valid = TRUE;
    for (const gchar *p = name; *p; p++) valid &= g_ascii_isxdigit (*p);
    if (!valid) continue;
    g_autofree gchar *path = g_build_filename (dir, name, NULL);
    GStatBuf st;
    if (g_lstat (path, &st) != 0 || !S_ISREG (st.st_mode)) continue;
    CacheFile *file = g_new0 (CacheFile, 1);
    file->path = g_steal_pointer (&path);
    file->time = st.st_mtime;
#ifdef __linux__
    file->nanoseconds = st.st_mtim.tv_nsec;
#endif
    file->size = st.st_size;
    g_ptr_array_add (files, file);
  }
  return files;
}
GBytes *spotifygtk_catalog_cache_get (const gchar *key, guint age)
{
  if (!key) return NULL;
  g_mutex_lock (&cache_lock);
  g_autofree gchar *path = cache_path (key);
  GStatBuf st;
  gchar *data = NULL;
  gsize size = 0;
  if (cache_enabled && g_lstat (path, &st) == 0 && S_ISREG (st.st_mode) &&
      st.st_size > 0 && st.st_size <= ENTRY_LIMIT &&
      (!age || g_get_real_time () / G_USEC_PER_SEC - st.st_mtime <= age))
    g_file_get_contents (path, &data, &size, NULL);
  g_mutex_unlock (&cache_lock);
  if (!data) return NULL;
  if (size > ENTRY_LIMIT) { g_free (data); return NULL; }
  return g_bytes_new_take (data, size);
}
void spotifygtk_catalog_cache_put (const gchar *key, GBytes *bytes, guint epoch)
{
  if (!key || !bytes || !g_bytes_get_size (bytes) ||
      g_bytes_get_size (bytes) > ENTRY_LIMIT) return;
  g_mutex_lock (&cache_lock);
  if (cache_enabled && epoch == cache_epoch) {
    g_autofree gchar *dir = cache_dir ();
    g_autofree gchar *path = cache_path (key);
    g_mkdir_with_parents (dir, 0700);
    gsize size;
    const gchar *data = g_bytes_get_data (bytes, &size);
    g_file_set_contents_full (path, data, size,
      G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
    g_autoptr(GPtrArray) files = cache_files ();
    g_ptr_array_sort (files, oldest_first);
    gsize total = 0;
    for (guint i = 0; i < files->len; i++) total += ((CacheFile *) files->pdata[i])->size;
    guint remaining = files->len;
    for (guint i = 0; i < files->len &&
         (total > DISK_LIMIT || remaining > FILE_LIMIT); i++) {
      CacheFile *file = files->pdata[i];
      /* Coarse filesystem timestamps can tie. The entry just written must
       * survive its own insertion, especially a playlist revision marker. */
      if (g_strcmp0 (file->path, path) == 0) continue;
      if (g_unlink (file->path) == 0) {
        total -= file->size;
        remaining--;
      }
    }
  }
  g_mutex_unlock (&cache_lock);
}
void spotifygtk_catalog_cache_remove (const gchar *key)
{
  if (!key) return;
  g_mutex_lock (&cache_lock);
  g_autofree gchar *path = cache_path (key);
  g_unlink (path);
  g_mutex_unlock (&cache_lock);
}
void spotifygtk_catalog_cache_clear (void)
{
  g_mutex_lock (&cache_lock);
  cache_epoch++; /* outstanding requests cannot repopulate cleared snapshots */
  g_autoptr(GPtrArray) files = cache_files ();
  for (guint i = 0; i < files->len; i++)
    g_unlink (((CacheFile *) files->pdata[i])->path);
  g_mutex_unlock (&cache_lock);
}
guint spotifygtk_catalog_cache_epoch (void)
{
  g_mutex_lock (&cache_lock);
  guint epoch = cache_epoch;
  g_mutex_unlock (&cache_lock);
  return epoch;
}
void spotifygtk_catalog_cache_set_enabled (gboolean enabled)
{
  g_mutex_lock (&cache_lock);
  cache_enabled = enabled;
  g_mutex_unlock (&cache_lock);
}
gchar *spotifygtk_catalog_context_key (const gchar *uri, guint limit)
{
  g_autofree gchar *revision_key = g_strconcat ("revision:", uri, NULL);
  g_autoptr(GBytes) revision = spotifygtk_catalog_cache_get (revision_key, 0);
  g_autofree gchar *digest = revision
    ? g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, revision) : g_strdup ("initial");
  return g_strdup_printf ("tracks:%s:%u:%s", uri, limit, digest);
}
void spotifygtk_catalog_context_invalidate (const gchar *uri)
{
  if (!uri) return;
  /* Change the revision for every track limit, including cover-only reads.
   * Epoch invalidation also prevents an older in-flight load writing it back. */
  g_mutex_lock (&cache_lock);
  guint epoch = ++cache_epoch;
  g_mutex_unlock (&cache_lock);
  g_autofree gchar *key = g_strconcat ("revision:", uri, NULL);
  g_autofree gchar *revision = g_uuid_string_random ();
  g_autoptr(GBytes) bytes = g_bytes_new (revision, strlen (revision));
  spotifygtk_catalog_cache_put (key, bytes, epoch);
}
gboolean spotifygtk_catalog_card_get (const gchar *uri, gchar **name, gchar **cover)
{
  *name = NULL; *cover = NULL;
  g_autofree gchar *key = g_strconcat ("card:", uri, NULL);
  g_autoptr(GBytes) bytes = spotifygtk_catalog_cache_get (key, 0);
  if (!bytes) return FALSE;
  g_autoptr(GVariant) value = g_variant_ref_sink (
    g_variant_new_from_bytes (G_VARIANT_TYPE ("(ss)"), bytes, FALSE));
  if (!g_variant_is_normal_form (value)) return FALSE;
  const gchar *title, *art;
  g_variant_get (value, "(&s&s)", &title, &art);
  if (!*title || g_str_has_prefix (title, "spotify:")) return FALSE;
  *name = g_strdup (title);
  *cover = *art ? g_strdup (art) : NULL;
  return TRUE;
}
void spotifygtk_catalog_card_put (const gchar *uri, const gchar *name,
                                  const gchar *cover, guint epoch)
{
  if (!uri || !name || !*name || !g_utf8_validate (name, -1, NULL) ||
      strlen (name) > 4096 || g_str_has_prefix (name, "spotify:")) return;
  /* Preserve artwork if the name request succeeds but the image lookup fails. */
  g_autofree gchar *old_name = NULL, *old_cover = NULL;
  if (!cover) spotifygtk_catalog_card_get (uri, &old_name, &old_cover);
  g_autoptr(GVariant) value = g_variant_ref_sink (
    g_variant_new ("(ss)", name, cover ? cover : old_cover ? old_cover : ""));
  g_autoptr(GBytes) bytes = g_variant_get_data_as_bytes (value);
  g_autofree gchar *key = g_strconcat ("card:", uri, NULL);
  spotifygtk_catalog_cache_put (key, bytes, epoch);
}

gboolean spotifygtk_catalog_card_is_fresh (const gchar *uri, guint age)
{
  g_autofree gchar *key = g_strconcat ("card:", uri, NULL);
  g_autoptr(GBytes) bytes = spotifygtk_catalog_cache_get (key, age);
  return bytes != NULL;
}

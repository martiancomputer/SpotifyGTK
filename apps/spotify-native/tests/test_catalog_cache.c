#include "spotify/catalog_cache.h"
#include "spotify/catalog_snapshot.h"
#include <glib/gstdio.h>

/* Snapshot ownership tests do not start an authenticated session. */
void spotifygtk_native_track_free (SpotifyNativeTrack *t)
{
  if (!t) return;
  g_free (t->uri); g_free (t->name); g_free (t->artists); g_free (t->album);
  g_free (t->cover_id); g_free (t->cover_id_small); g_free (t->album_uri);
  g_free (t->artist_uri); g_free (t->section_title); g_free (t->section_detail);
  g_free (t);
}
void spotifygtk_native_release_free (SpotifyNativeRelease *r)
{
  if (!r) return;
  g_free (r->uri); g_free (r->name); g_free (r->cover_id);
  g_clear_pointer (&r->tracks, g_ptr_array_unref); g_free (r);
}
static void test_identity (void)
{
  const gchar *uri = "spotify:playlist:fixture";
  guint epoch = spotifygtk_catalog_cache_epoch ();
  spotifygtk_catalog_card_put (uri, "Weekly playlist", "art-id", epoch);
  spotifygtk_catalog_card_put (uri, uri, NULL, epoch);
  g_autofree gchar *name = NULL, *art = NULL;
  g_assert_true (spotifygtk_catalog_card_get (uri, &name, &art));
  g_assert_cmpstr (name, ==, "Weekly playlist");
  g_assert_cmpstr (art, ==, "art-id");
  spotifygtk_catalog_card_put (uri, "Renamed playlist", NULL, epoch);
  g_clear_pointer (&name, g_free); g_clear_pointer (&art, g_free);
  g_assert_true (spotifygtk_catalog_card_get (uri, &name, &art));
  g_assert_cmpstr (name, ==, "Renamed playlist");
  g_assert_cmpstr (art, ==, "art-id");
}
static void test_restart (void)
{
  const gchar *uri = "spotify:artist:fixture";
  if (g_test_subprocess ()) {
    g_autofree gchar *name = NULL, *art = NULL;
    g_assert_true (spotifygtk_catalog_card_get (uri, &name, &art));
    g_assert_cmpstr (name, ==, "Artist fixture");
    g_assert_cmpstr (art, ==, "portrait-id");
    return;
  }
  spotifygtk_catalog_card_put (uri, "Artist fixture", "portrait-id",
    spotifygtk_catalog_cache_epoch ());
  g_test_trap_subprocess (NULL, 5 * G_USEC_PER_SEC, 0);
  g_test_trap_assert_passed ();
}
static void test_clear_and_disable (void)
{
  guint old = spotifygtk_catalog_cache_epoch ();
  spotifygtk_catalog_cache_clear ();
  spotifygtk_catalog_card_put ("spotify:playlist:late", "Late reply", "art", old);
  g_autofree gchar *name = NULL, *art = NULL;
  g_assert_false (spotifygtk_catalog_card_get ("spotify:playlist:late", &name, &art));
  spotifygtk_catalog_cache_set_enabled (FALSE);
  spotifygtk_catalog_card_put ("spotify:playlist:disabled", "Disabled", "art",
    spotifygtk_catalog_cache_epoch ());
  spotifygtk_catalog_cache_set_enabled (TRUE);
  g_assert_false (spotifygtk_catalog_card_get ("spotify:playlist:disabled", &name, &art));
}
static void test_limits (void)
{
  g_autofree gchar *payload = g_malloc0 (4 * 1024 * 1024 + 1);
  g_autoptr(GBytes) oversized = g_bytes_new_static (payload, 4 * 1024 * 1024 + 1);
  spotifygtk_catalog_cache_put ("oversized", oversized, spotifygtk_catalog_cache_epoch ());
  g_assert_null (spotifygtk_catalog_cache_get ("oversized", 0));
  for (guint i = 0; i < 270; i++) {
    g_autofree gchar *key = g_strdup_printf ("bounded:%u", i);
    g_autoptr(GBytes) bytes = g_bytes_new_static ("ok", 2);
    spotifygtk_catalog_cache_put (key, bytes, spotifygtk_catalog_cache_epoch ());
    g_autoptr(GBytes) inserted = spotifygtk_catalog_cache_get (key, 0);
    g_assert_nonnull (inserted);
  }
  g_autofree gchar *path = g_build_filename (g_get_user_cache_dir (), "spotifygtk", "catalog-v1", NULL);
  g_autoptr(GDir) dir = g_dir_open (path, 0, NULL);
  guint count = 0;
  while (g_dir_read_name (dir)) count++;
  g_assert_cmpuint (count, <=, 256);
}
static void test_revision (void)
{
  g_autofree gchar *first = spotifygtk_catalog_context_key ("spotify:playlist:fixture", 200);
  g_autofree gchar *preview = spotifygtk_catalog_context_key ("spotify:playlist:fixture", 1);
  spotifygtk_catalog_context_invalidate ("spotify:playlist:fixture");
  g_autofree gchar *next = spotifygtk_catalog_context_key ("spotify:playlist:fixture", 200);
  g_autofree gchar *next_preview = spotifygtk_catalog_context_key ("spotify:playlist:fixture", 1);
  g_assert_cmpstr (first, !=, next);
  g_assert_cmpstr (preview, !=, next_preview);
  /* The limits test fills this cache. A subsequent insertion must not evict
   * the new revision while older snapshots are still present. */
  g_autoptr(GBytes) bytes = g_bytes_new_static ("newer", 5);
  spotifygtk_catalog_cache_put ("after-revision", bytes,
    spotifygtk_catalog_cache_epoch ());
  g_autofree gchar *after_insert = spotifygtk_catalog_context_key (
    "spotify:playlist:fixture", 200);
  g_assert_cmpstr (next, ==, after_insert);
}
static void test_snapshots (void)
{
  SpotifyNativeTrack t = { .uri = "spotify:track:fixture", .name = "Fixture",
    .artists = "Artist", .album = "Album", .duration_ms = 123456,
    .is_explicit = TRUE, .cover_id = "large", .cover_id_small = "small",
    .artist_uri = "spotify:artist:fixture", .album_uri = "spotify:album:fixture",
    .release_year = 2026 };
  g_autoptr(GPtrArray) tracks = g_ptr_array_new ();
  g_ptr_array_add (tracks, &t);
  g_ptr_array_add (tracks, &t); /* preserve duplicate recordings/order */
  g_autoptr(GBytes) bytes = spotifygtk_catalog_tracks_pack (tracks);
  g_autoptr(GPtrArray) copy = spotifygtk_catalog_tracks_unpack (bytes);
  g_assert_nonnull (copy);
  g_assert_cmpuint (copy->len, ==, 2);
  SpotifyNativeTrack *out = copy->pdata[1];
  g_assert_cmpstr (out->name, ==, t.name);
  g_assert_cmpint (out->duration_ms, ==, t.duration_ms);
  g_assert_true (out->is_explicit);
  g_assert_cmpstr (out->cover_id_small, ==, "small");
  SpotifyNativeRelease r = { .uri = "spotify:album:fixture", .name = "Album",
    .cover_id = "art", .year = 2026, .type = SPOTIFY_ALBUM_TYPE_ALBUM,
    .tracks = tracks };
  g_autoptr(GPtrArray) releases = g_ptr_array_new ();
  g_ptr_array_add (releases, &r);
  g_autoptr(GBytes) packed = spotifygtk_catalog_releases_pack (releases);
  g_autoptr(GPtrArray) unpacked = spotifygtk_catalog_releases_unpack (packed);
  g_assert_nonnull (unpacked);
  SpotifyNativeRelease *release = unpacked->pdata[0];
  g_assert_cmpstr (release->name, ==, "Album");
  g_assert_cmpuint (release->tracks->len, ==, 2);
  g_assert_cmpint (release->year, ==, 2026);
}
static void test_corrupt (void)
{
  g_autoptr(GBytes) bytes = g_bytes_new_static ("\xff\xff\xff", 3);
  spotifygtk_catalog_cache_put ("card:spotify:playlist:corrupt", bytes,
    spotifygtk_catalog_cache_epoch ());
  g_autofree gchar *name = NULL, *art = NULL;
  g_assert_false (spotifygtk_catalog_card_get ("spotify:playlist:corrupt", &name, &art));
  g_assert_null (spotifygtk_catalog_tracks_unpack (bytes));
  g_assert_null (spotifygtk_catalog_releases_unpack (bytes));
}
int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!g_test_subprocess ()) {
    gchar *dir = g_dir_make_tmp ("spotifygtk-catalog-test-XXXXXX", NULL);
    g_assert_nonnull (dir);
    g_setenv ("XDG_CACHE_HOME", dir, TRUE);
    g_free (dir);
  }
  g_test_add_func ("/catalog/identity", test_identity);
  g_test_add_func ("/catalog/restart", test_restart);
  g_test_add_func ("/catalog/clear-disable", test_clear_and_disable);
  g_test_add_func ("/catalog/limits", test_limits);
  g_test_add_func ("/catalog/revision", test_revision);
  g_test_add_func ("/catalog/snapshots", test_snapshots);
  g_test_add_func ("/catalog/corrupt", test_corrupt);
  return g_test_run ();
}

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>

#include "ui/local_catalog.h"
#include "ui/settings.h"

/* This target links only the local index and settings modules, not a Spotify
 * session or network stack. The same track destructor contract is sufficient
 * for its owned presentation objects. */
void
spotifygtk_native_track_free (SpotifyNativeTrack *track)
{
  if (!track) return;
  g_free (track->uri);
  g_free (track->name);
  g_free (track->artists);
  g_free (track->album);
  g_free (track->cover_id);
  g_free (track->cover_id_small);
  g_free (track->album_uri);
  g_free (track->artist_uri);
  g_free (track->section_title);
  g_free (track->section_detail);
  g_free (track);
}

static gchar *music_dir;
static gchar *sandbox_dir;
static guint changed_count;

static void
on_catalog_changed (SpotifyGtkLocalCatalog *catalog, gpointer data)
{
  changed_count++;
  (void) catalog;
  (void) data;
}

static void
put_le16 (guchar *p, guint16 value)
{
  p[0] = value & 0xff;
  p[1] = value >> 8;
}

static void
put_le32 (guchar *p, guint32 value)
{
  p[0] = value & 0xff;
  p[1] = (value >> 8) & 0xff;
  p[2] = (value >> 16) & 0xff;
  p[3] = value >> 24;
}

static void
make_wav (const gchar *path)
{
  /* Ordinary 8 kHz mono signed PCM, one second of silence. */
  g_autofree guchar *bytes = g_malloc0 (44 + 16000);
  memcpy (bytes, "RIFF", 4);
  put_le32 (bytes + 4, 36 + 16000);
  memcpy (bytes + 8, "WAVEfmt ", 8);
  put_le32 (bytes + 16, 16);
  put_le16 (bytes + 20, 1);
  put_le16 (bytes + 22, 1);
  put_le32 (bytes + 24, 8000);
  put_le32 (bytes + 28, 16000);
  put_le16 (bytes + 32, 2);
  put_le16 (bytes + 34, 16);
  memcpy (bytes + 36, "data", 4);
  put_le32 (bytes + 40, 16000);
  g_assert_true (g_file_set_contents (path, (const gchar *) bytes,
                                      44 + 16000, NULL));
}

static gboolean
wait_for_tracks (guint expected)
{
  gint64 until = g_get_monotonic_time () + 8 * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < until) {
    while (g_main_context_iteration (NULL, FALSE)) {}
    SpotifyGtkLocalSnapshot *snapshot = spotifygtk_local_catalog_ref_snapshot ();
    const GPtrArray *albums = spotifygtk_local_snapshot_albums (snapshot);
    guint count = 0;
    for (guint i = 0; albums && i < albums->len; i++) {
      const SpotifyGtkLocalAlbum *album = g_ptr_array_index ((GPtrArray *) albums, i);
      count += album->tracks->len;
    }
    spotifygtk_local_snapshot_unref (snapshot);
    if (count == expected) return TRUE;
    g_usleep (10000);
  }
  return FALSE;
}

static gboolean
wait_for_change (guint previous)
{
  gint64 until = g_get_monotonic_time () + 8 * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < until) {
    while (g_main_context_iteration (NULL, FALSE)) {}
    if (changed_count > previous) return TRUE;
    g_usleep (10000);
  }
  return FALSE;
}

static void
check_encoded_imports (void)
{
  g_autofree gchar *ffmpeg = g_find_program_in_path ("ffmpeg");
  if (!ffmpeg) {
    g_test_message ("ffmpeg executable unavailable; encoded fixture check skipped");
    return;
  }
  static const gchar *names[] = { "sample.flac", "sample.opus", "sample.m4a" };
  static const gchar *encoders[] = { "flac", "libopus", "aac" };
  static const gchar *codecs[] = { "flac", "opus", "aac" };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
    g_autofree gchar *path = g_build_filename (music_dir, names[i], NULL);
    gchar *argv[] = { ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
      "anullsrc=r=48000:cl=stereo", "-t", "0.1", "-c:a",
      (gchar *) encoders[i], path, NULL };
    gint status = 0;
    g_assert_true (g_spawn_sync (NULL, argv, NULL, 0, NULL, NULL,
                                 NULL, NULL, &status, NULL));
    g_assert_true (g_spawn_check_wait_status (status, NULL));
  }
  g_assert_true (wait_for_tracks (4));
  SpotifyGtkLocalSnapshot *snapshot = spotifygtk_local_catalog_ref_snapshot ();
  const GPtrArray *albums = spotifygtk_local_snapshot_albums (snapshot);
  g_assert_cmpuint (albums->len, ==, 1);
  const SpotifyGtkLocalAlbum *album = g_ptr_array_index ((GPtrArray *) albums, 0);
  for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
    gboolean found = FALSE;
    for (guint j = 0; j < album->tracks->len; j++) {
      const SpotifyGtkLocalTrack *track = g_ptr_array_index (album->tracks, j);
      if (g_str_has_suffix (track->path, names[i])) {
        g_assert_cmpstr (track->codec, ==, codecs[i]);
        found = TRUE;
        break;
      }
    }
    g_assert_true (found);
    g_autofree gchar *path = g_build_filename (music_dir, names[i], NULL);
    g_assert_cmpint (g_remove (path), ==, 0);
  }
  spotifygtk_local_snapshot_unref (snapshot);
  g_assert_true (wait_for_tracks (1));
}

static void
test_catalog_reconciliation (void)
{
  g_autofree gchar *first = g_build_filename (music_dir, "one.wav", NULL);
  g_autofree gchar *second = g_build_filename (music_dir, "two.wav", NULL);
  g_autofree gchar *broken = g_build_filename (music_dir, "broken.wav", NULL);
  make_wav (first);
  g_assert_true (g_file_set_contents (broken, "RIFF", 4, NULL));
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  g_assert_true (spotifygtk_settings_add_local_directory (settings, music_dir));
  SpotifyGtkLocalCatalog *catalog = spotifygtk_local_catalog_get_default ();
  g_signal_connect (catalog, "changed", G_CALLBACK (on_catalog_changed), NULL);
  g_assert_true (wait_for_tracks (1));

  SpotifyGtkLocalSnapshot *old = spotifygtk_local_catalog_ref_snapshot ();
  const GPtrArray *albums = spotifygtk_local_snapshot_albums (old);
  g_assert_cmpuint (albums->len, ==, 1);
  const SpotifyGtkLocalAlbum *album = g_ptr_array_index ((GPtrArray *) albums, 0);
  g_assert_cmpstr (album->title, ==, "music");
  g_assert_true (g_str_has_prefix (album->uri, "local:album:"));
  const SpotifyGtkLocalTrack *track = g_ptr_array_index (album->tracks, 0);
  g_assert_cmpint (track->display->duration_ms, ==, 1000);
  g_assert_true (g_str_has_prefix (track->display->uri, "local:track:"));
  g_autofree gchar *uri = g_strdup (track->display->uri);

  make_wav (second);
  g_assert_true (wait_for_tracks (2));
  g_assert_cmpstr (track->display->uri, ==, uri); /* old snapshot still owns it */
  spotifygtk_local_snapshot_unref (old);

  g_assert_cmpint (g_remove (second), ==, 0);
  g_assert_true (wait_for_tracks (1));
  check_encoded_imports ();
  g_autofree gchar *moved = g_build_filename (music_dir, "renamed.wav", NULL);
  guint renamed_from = changed_count;
  g_assert_cmpint (g_rename (first, moved), ==, 0);
  spotifygtk_local_catalog_refresh (catalog);
  g_assert_true (wait_for_change (renamed_from));
  g_autofree gchar *resolved = spotifygtk_local_catalog_dup_track_path (uri);
  g_assert_cmpstr (resolved, ==, moved);
  renamed_from = changed_count;
  g_assert_cmpint (g_rename (moved, first), ==, 0);
  spotifygtk_local_catalog_refresh (catalog);
  g_assert_true (wait_for_change (renamed_from));
  g_autofree gchar *restored = spotifygtk_local_catalog_dup_track_path (uri);
  g_assert_cmpstr (restored, ==, first);
  g_autofree gchar *index = g_build_filename (
    g_get_user_data_dir (), "spotifygtk", "local-index-v1", NULL);
  g_assert_true (g_file_test (index, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_set_contents (index, "corrupt", -1, NULL));
  guint before = changed_count;
  spotifygtk_local_catalog_refresh (catalog);
  g_assert_true (wait_for_change (before));
  g_assert_true (wait_for_tracks (1));
  g_autofree gchar *repaired = NULL;
  g_assert_true (g_file_get_contents (index, &repaired, NULL, NULL));
  g_assert_true (g_str_has_prefix (repaired, "SGTKLOCAL1\n"));
  /* A missing file must disappear from the playable index. Device favorites
   * and playlist references keep the stable URI, ready if it reappears. */
  g_assert_cmpint (g_remove (first), ==, 0);
  g_assert_true (wait_for_tracks (0));
  g_autofree gchar *missing = spotifygtk_local_catalog_dup_track_path (uri);
  g_assert_null (missing);
  g_assert_true (spotifygtk_settings_remove_local_directory (settings, music_dir));
  g_assert_true (wait_for_tracks (0));
  g_assert_cmpint (g_remove (broken), ==, 0);
}

int
main (int argc, char **argv)
{
  sandbox_dir = g_dir_make_tmp ("spotifygtk-local-test-XXXXXX", NULL);
  g_assert_nonnull (sandbox_dir);
  g_autofree gchar *config = g_build_filename (sandbox_dir, "config", NULL);
  g_autofree gchar *cache = g_build_filename (sandbox_dir, "cache", NULL);
  g_autofree gchar *data = g_build_filename (sandbox_dir, "data", NULL);
  music_dir = g_build_filename (sandbox_dir, "music", NULL);
  g_assert_cmpint (g_mkdir_with_parents (music_dir, 0700), ==, 0);
  g_setenv ("XDG_CONFIG_HOME", config, TRUE);
  g_setenv ("XDG_CACHE_HOME", cache, TRUE);
  g_setenv ("XDG_DATA_HOME", data, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/local/catalog-reconciliation", test_catalog_reconciliation);
  gint result = g_test_run ();
  g_free (music_dir);
  g_free (sandbox_dir);
  return result;
}

#include "ui/device_playlists.h"
#include <glib/gstdio.h>

void
spotifygtk_native_track_free (SpotifyNativeTrack *track)
{
  if (!track) return;
  g_free (track->uri);
  g_free (track->name);
  g_free (track->artists);
  g_free (track->album);
  g_free (track->cover_id);
  g_free (track->section_detail);
  g_free (track);
}

SpotifyNativeTrack *
spotifygtk_native_track_copy (const SpotifyNativeTrack *track)
{
  SpotifyNativeTrack *copy = g_new0 (SpotifyNativeTrack, 1);
  copy->uri = g_strdup (track->uri);
  copy->name = g_strdup (track->name);
  copy->artists = g_strdup (track->artists);
  copy->album = g_strdup (track->album);
  copy->cover_id = g_strdup (track->cover_id);
  copy->section_detail = g_strdup (track->section_detail);
  copy->duration_ms = track->duration_ms;
  return copy;
}

static gchar *test_dir;

static void
wait_for_save (const gchar *needle)
{
  g_autofree gchar *path = g_build_filename (test_dir, "spotifygtk",
                                             "device-playlists.ini", NULL);
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline) {
    while (g_main_context_pending (NULL))
      g_main_context_iteration (NULL, FALSE);
    g_autofree gchar *data = NULL;
    if (g_file_get_contents (path, &data, NULL, NULL) &&
        g_strstr_len (data, -1, needle)) return;
    g_usleep (10000);
  }
  g_error ("device playlist save timed out");
}

static void
test_mixed_playlist_and_overlay (void)
{
  SpotifyNativeTrack local = { .uri = "local:track:one", .name = "Local",
    .artists = "Artist", .album = "Album", .cover_id = "", .duration_ms = 123 };
  SpotifyNativeTrack remote = { .uri = "spotify:track:one", .name = "Spotify",
    .artists = "Artist", .album = "Album", .cover_id = "", .duration_ms = 456 };
  SpotifyGtkDevicePlaylists *store = spotifygtk_device_playlists_new ();
  g_autofree gchar *uri = spotifygtk_device_playlists_create (store, "Mixed");
  g_assert_nonnull (uri);
  g_assert_true (spotifygtk_device_playlists_add (store, uri, &local));
  g_assert_true (spotifygtk_device_playlists_add (store, uri, &remote));
  g_assert_true (spotifygtk_device_playlists_add (store, uri, &local));
  g_autoptr(GPtrArray) mixed = spotifygtk_device_playlists_tracks (store, uri, NULL);
  g_assert_cmpuint (mixed->len, ==, 3);
  g_assert_cmpstr (((SpotifyNativeTrack *)g_ptr_array_index (mixed, 1))->uri,
                   ==, remote.uri);
  g_assert_true (spotifygtk_device_playlists_remove_at (store, uri, 2, local.uri));
  g_assert_false (spotifygtk_device_playlists_remove_at (store, uri, 1, local.uri));

  const gchar *spotify_list = "spotify:playlist:example";
  g_assert_false (spotifygtk_device_playlists_add (store, spotify_list, &remote));
  g_assert_true (spotifygtk_device_playlists_add (store, spotify_list, &local));
  spotifygtk_device_playlists_set_overlay_name (store, spotify_list, "Remote with local");
  g_autoptr(GPtrArray) listed = spotifygtk_device_playlists_list (store);
  gboolean found_overlay = FALSE;
  gint64 created_at = 0;
  for (guint i = 0; i < listed->len; i++) {
    SpotifyGtkDevicePlaylistInfo *info = g_ptr_array_index (listed, i);
    if (g_strcmp0 (info->uri, uri) == 0) {
      created_at = info->added_at;
      g_assert_cmpint (created_at, >, 0);
    }
    if (g_strcmp0 (info->uri, spotify_list) == 0) {
      g_assert_true (info->overlay);
      g_assert_cmpstr (info->name, ==, "Remote with local");
      found_overlay = TRUE;
    }
  }
  g_assert_true (found_overlay);
  g_assert_cmpint (created_at, >, 0);
  g_autoptr(GPtrArray) server = g_ptr_array_new ();
  g_ptr_array_add (server, &remote);
  g_autoptr(GPtrArray) visible = spotifygtk_device_playlists_tracks (
    store, spotify_list, server);
  g_assert_cmpuint (visible->len, ==, 2);
  g_assert_cmpstr (((SpotifyNativeTrack *)g_ptr_array_index (visible, 0))->uri,
                   ==, remote.uri);
  g_assert_cmpstr (((SpotifyNativeTrack *)g_ptr_array_index (visible, 1))->uri,
                   ==, local.uri);
  spotifygtk_device_playlists_free (store);
  wait_for_save (local.uri);

  store = spotifygtk_device_playlists_new ();
  g_autoptr(GPtrArray) restored = spotifygtk_device_playlists_tracks (store, uri, NULL);
  g_assert_cmpuint (restored->len, ==, 2);
  g_autoptr(GPtrArray) overlay = spotifygtk_device_playlists_tracks (
    store, spotify_list, server);
  g_assert_cmpuint (overlay->len, ==, 2);
  g_autoptr(GPtrArray) offline_listed = spotifygtk_device_playlists_list (store);
  found_overlay = FALSE;
  for (guint i = 0; i < offline_listed->len; i++) {
    SpotifyGtkDevicePlaylistInfo *info = g_ptr_array_index (offline_listed, i);
    if (g_strcmp0 (info->uri, uri) == 0)
      g_assert_cmpint (info->added_at, ==, created_at);
    if (g_strcmp0 (info->uri, spotify_list) == 0) {
      g_assert_true (info->overlay);
      g_assert_cmpstr (info->name, ==, "Remote with local");
      found_overlay = TRUE;
    }
  }
  g_assert_true (found_overlay);
  g_assert_false (spotifygtk_device_playlists_delete (store, spotify_list));
  g_assert_true (spotifygtk_device_playlists_delete (store, uri));
  g_autoptr(GPtrArray) after_delete = spotifygtk_device_playlists_tracks (store, uri, NULL);
  g_assert_cmpuint (after_delete->len, ==, 0);
  spotifygtk_device_playlists_free (store);
}

static void
test_overlay_position_survives_refresh (void)
{
  const gchar *uri = "spotify:playlist:positions";
  SpotifyNativeTrack a = { .uri = "spotify:track:a" };
  SpotifyNativeTrack b = { .uri = "spotify:track:b" };
  SpotifyNativeTrack c = { .uri = "spotify:track:c" };
  SpotifyNativeTrack local = { .uri = "local:track:position", .name = "Local",
    .artists = "Artist", .album = "Album", .cover_id = "" };
  SpotifyGtkDevicePlaylists *store = spotifygtk_device_playlists_new ();
  g_assert_true (spotifygtk_device_playlists_add (store, uri, &local));
  g_assert_true (spotifygtk_device_playlists_add (store, uri, &local));
  g_autoptr(GPtrArray) offline = spotifygtk_device_playlists_tracks (store, uri, NULL);
  g_assert_cmpuint (offline->len, ==, 2);
  g_autoptr(GPtrArray) server = g_ptr_array_new ();
  g_ptr_array_add (server, &a); g_ptr_array_add (server, &b); g_ptr_array_add (server, &b);
  g_autoptr(GPtrArray) first = spotifygtk_device_playlists_tracks (store, uri, server);
  g_assert_cmpuint (first->len, ==, 5);
  spotifygtk_device_playlists_free (store);
  store = spotifygtk_device_playlists_new ();
  g_ptr_array_add (server, &c);
  g_autoptr(GPtrArray) refreshed = spotifygtk_device_playlists_tracks (store, uri, server);
  g_assert_cmpstr (((SpotifyNativeTrack *) refreshed->pdata[3])->uri, ==, local.uri);
  g_assert_cmpstr (((SpotifyNativeTrack *) refreshed->pdata[4])->uri, ==, local.uri);
  g_assert_cmpstr (((SpotifyNativeTrack *) refreshed->pdata[5])->uri, ==, c.uri);
  /* Removing an anchor has a deterministic bounded-position fallback. */
  g_ptr_array_remove_index (server, 2);
  g_autoptr(GPtrArray) missing = spotifygtk_device_playlists_tracks (store, uri, server);
  g_assert_cmpstr (((SpotifyNativeTrack *) missing->pdata[3])->uri, ==, local.uri);
  g_assert_true (spotifygtk_device_playlists_remove (store, uri, local.uri, 1));
  spotifygtk_device_playlists_free (store);
}

typedef struct { SpotifyGtkDevicePlaylists *store; gboolean done, ok; GError *error; } ArtResult;
static void
art_saved (GObject *source, GAsyncResult *result, gpointer data)
{
  ArtResult *art = data;
  art->ok = spotifygtk_device_playlists_set_artwork_finish (art->store, result, &art->error);
  art->done = TRUE; (void) source;
}
static void
test_playlist_artwork_persistence (void)
{
  SpotifyGtkDevicePlaylists *store = spotifygtk_device_playlists_new ();
  g_autofree gchar *uri = spotifygtk_device_playlists_create (store, "Artwork fixture");
  /* The store preserves bounded encoder output verbatim; codec correctness is
   * covered by the crop editor test, not duplicated by this storage test. */
  const guint8 payload[] = { 0xff, 0xd8, 0xff, 0xd9 };
  g_autoptr(GBytes) jpeg = g_bytes_new_static (payload, sizeof payload);
  ArtResult result = { .store = store };
  spotifygtk_device_playlists_set_artwork (store, uri, jpeg, NULL, art_saved, &result);
  gint64 end = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (!result.done && g_get_monotonic_time () < end) { g_main_context_iteration (NULL, FALSE); g_usleep (1000); }
  g_assert_true (result.done); g_assert_true (result.ok); g_assert_no_error (result.error);
  wait_for_save ("artwork=");
  spotifygtk_device_playlists_free (store);
  store = spotifygtk_device_playlists_new ();
  g_autoptr(GPtrArray) lists = spotifygtk_device_playlists_list (store);
  gchar *cover = NULL;
  for (guint i = 0; i < lists->len; i++) {
    SpotifyGtkDevicePlaylistInfo *info = lists->pdata[i];
    if (g_strcmp0 (info->uri, uri) == 0) cover = info->cover_id;
  }
  g_assert_nonnull (cover);
  g_autofree gchar *path = spotifygtk_device_playlists_dup_art_path (cover);
  g_autofree gchar *contents = NULL; gsize size = 0;
  g_assert_true (g_file_get_contents (path, &contents, &size, NULL));
  g_assert_cmpmem (contents, size, payload, sizeof payload);
  g_assert_null (spotifygtk_device_playlists_dup_art_path ("local:playlist-art:../../file"));
  g_assert_true (spotifygtk_device_playlists_delete (store, uri));
  end = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (g_file_test (path, G_FILE_TEST_EXISTS) && g_get_monotonic_time () < end) { g_main_context_iteration (NULL, FALSE); g_usleep (1000); }
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  spotifygtk_device_playlists_free (store);
}

int
main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  test_dir = g_dir_make_tmp ("spotifygtk-playlists-test-XXXXXX", &error);
  g_assert_no_error (error);
  g_setenv ("XDG_DATA_HOME", test_dir, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/device-playlists/mixed-and-overlay", test_mixed_playlist_and_overlay);
  g_test_add_func ("/device-playlists/stable-position", test_overlay_position_survives_refresh);
  g_test_add_func ("/device-playlists/artwork-persistence", test_playlist_artwork_persistence);
  int result = g_test_run ();
  g_free (test_dir);
  return result;
}

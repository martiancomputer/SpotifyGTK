#include "ui/local_favorites.h"
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
  copy->duration_ms = track->duration_ms;
  copy->liked_at = track->liked_at;
  return copy;
}

static gchar *test_dir;

static void
capture_date (gpointer key, gpointer value, gpointer data)
{
  *(gint64 *) data = ((SpotifyNativeTrack *) value)->liked_at;
  (void) key;
}

static void
test_local_favorite_persists_and_rejects_spotify (void)
{
  SpotifyNativeTrack local = {
    .uri = "local:track:abc", .name = "Example", .artists = "Artist",
    .album = "Album", .cover_id = "", .duration_ms = 12345
  };
  SpotifyNativeTrack remote = { .uri = "spotify:track:abc" };
  SpotifyGtkLocalFavorites *favorites = spotifygtk_local_favorites_new ();
  g_assert_false (spotifygtk_local_favorites_set (favorites, &remote, TRUE));
  g_assert_true (spotifygtk_local_favorites_set (favorites, &local, TRUE));
  g_assert_true (spotifygtk_local_favorites_contains (favorites, local.uri));
  gint64 liked_at = 0;
  spotifygtk_local_favorites_foreach (favorites, capture_date, &liked_at);
  g_assert_cmpint (liked_at, >, 0);
  local.liked_at = liked_at + 86400;
  g_assert_true (spotifygtk_local_favorites_set (favorites, &local, TRUE));
  gint64 updated_at = 0;
  spotifygtk_local_favorites_foreach (favorites, capture_date, &updated_at);
  g_assert_cmpint (updated_at, ==, liked_at);
  spotifygtk_local_favorites_free (favorites);

  g_autofree gchar *path = g_build_filename (test_dir, "spotifygtk",
                                            "local-favorites.ini", NULL);
  gint64 deadline = g_get_monotonic_time () + 2 * G_USEC_PER_SEC;
  gboolean persisted = FALSE;
  while (!persisted && g_get_monotonic_time () < deadline) {
    while (g_main_context_pending (NULL))
      g_main_context_iteration (NULL, FALSE);
    g_autofree gchar *data = NULL;
    if (g_file_get_contents (path, &data, NULL, NULL) &&
        g_strstr_len (data, -1, local.uri))
      persisted = TRUE;
    g_usleep (10000);
  }
  g_assert_true (persisted);

  favorites = spotifygtk_local_favorites_new ();
  g_assert_true (spotifygtk_local_favorites_contains (favorites, local.uri));
  updated_at = 0;
  spotifygtk_local_favorites_foreach (favorites, capture_date, &updated_at);
  g_assert_cmpint (updated_at, ==, liked_at);
  g_assert_true (spotifygtk_local_favorites_set (favorites, &local, FALSE));
  g_assert_false (spotifygtk_local_favorites_contains (favorites, local.uri));
  spotifygtk_local_favorites_free (favorites);
}

int
main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  test_dir = g_dir_make_tmp ("spotifygtk-favorites-test-XXXXXX", &error);
  g_assert_no_error (error);
  g_setenv ("XDG_DATA_HOME", test_dir, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/local-favorites/persistence", test_local_favorite_persists_and_rejects_spotify);
  int result = g_test_run ();
  g_free (test_dir);
  return result;
}

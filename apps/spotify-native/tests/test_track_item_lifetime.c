#include "ui/track_item.h"
#include <gio/gio.h>
#include <string.h>

static SpotifyNativeTrack *source_track (void)
{
  SpotifyNativeTrack *track = g_new0 (SpotifyNativeTrack, 1);
  track->uri = g_strdup ("spotify:track:fixture");
  track->name = g_strdup ("Track fixture");
  track->artists = g_strdup ("Artist fixture");
  track->album = g_strdup ("Album fixture");
  track->cover_id = g_strdup ("large-cover");
  track->cover_id_small = g_strdup ("small-cover");
  track->album_uri = g_strdup ("spotify:album:fixture");
  track->artist_uri = g_strdup ("spotify:artist:fixture");
  track->section_title = g_strdup ("Release fixture");
  track->section_detail = g_strdup ("2026");
  track->duration_ms = 225000;
  track->liked_at = 12345;
  track->release_year = 2026;
  track->device_index = 2;
  track->is_explicit = TRUE;
  return track;
}

static void source_free (SpotifyNativeTrack *track)
{
  g_free (track->uri); g_free (track->name);
  g_free (track->artists); g_free (track->album);
  g_free (track->cover_id); g_free (track->cover_id_small);
  g_free (track->album_uri); g_free (track->artist_uri);
  g_free (track->section_title); g_free (track->section_detail);
  memset (track, 0xa5, sizeof *track);
  g_free (track);
}

static void assert_metadata (SpotifyGtkTrackItem *item)
{
  const SpotifyNativeTrack *track = spotifygtk_track_item_get_track (item);
  g_assert_cmpstr (track->uri, ==, "spotify:track:fixture");
  g_assert_cmpstr (track->name, ==, "Track fixture");
  g_assert_cmpstr (track->artists, ==, "Artist fixture");
  g_assert_cmpstr (track->album, ==, "Album fixture");
  g_assert_cmpstr (track->cover_id, ==, "large-cover");
  g_assert_cmpstr (track->cover_id_small, ==, "small-cover");
  g_assert_cmpstr (track->album_uri, ==, "spotify:album:fixture");
  g_assert_cmpstr (track->artist_uri, ==, "spotify:artist:fixture");
  g_assert_cmpstr (track->section_title, ==, "Release fixture");
  g_assert_cmpstr (track->section_detail, ==, "2026");
  g_assert_cmpint (track->duration_ms, ==, 225000);
  g_assert_cmpint (track->liked_at, ==, 12345);
  g_assert_cmpuint (track->device_index, ==, 2);
  g_assert_cmpuint (track->release_year, ==, 2026);
  g_assert_true (track->is_explicit);
}

static void test_source_released (void)
{
  SpotifyNativeTrack *track = source_track ();
  g_autoptr(SpotifyGtkTrackItem) item = spotifygtk_track_item_new (track, 7);
  source_free (track);
  assert_metadata (item);
  g_assert_cmpuint (spotifygtk_track_item_get_number (item), ==, 7);
}

static void test_retired_focus_item (void)
{
  /* Reproduce the ownership sequence from the core without GTK widgets:
   * model removal, source collection freed, delayed focused-item cleanup. */
  g_autoptr(GListStore) model = g_list_store_new (SPOTIFYGTK_TYPE_TRACK_ITEM);
  SpotifyNativeTrack *source = source_track ();
  SpotifyGtkTrackItem *item = spotifygtk_track_item_new (source, 1);
  g_list_store_append (model, item);
  g_object_unref (item);
  g_autoptr(SpotifyGtkTrackItem) focused = g_list_model_get_item (G_LIST_MODEL (model), 0);
  g_list_store_remove_all (model);
  source_free (source);
  assert_metadata (focused);
  /* This comparison was the exact dereference that faulted in unbind. */
  g_assert_cmpint (g_strcmp0 (spotifygtk_track_item_get_uri (focused),
                             "spotifygtk:page-header"), !=, 0);
  gpointer survivor = focused;
  g_object_add_weak_pointer (G_OBJECT (focused), &survivor);
  g_clear_object (&focused);
  g_assert_null (survivor);
}

static void test_replacement_shares_strings (void)
{
  SpotifyNativeTrack *source = source_track ();
  g_autoptr(SpotifyGtkTrackItem) old = spotifygtk_track_item_new (source, 1);
  g_autoptr(SpotifyGtkTrackItem) replacement = spotifygtk_track_item_new (source, 2);
  const SpotifyNativeTrack *a = spotifygtk_track_item_get_track (old);
  const SpotifyNativeTrack *b = spotifygtk_track_item_get_track (replacement);
  g_assert_true (a != b);
  g_assert_true (a->uri == b->uri);
  g_assert_true (a->name == b->name);
  g_assert_true (a->album == b->album);
  g_assert_true (a->cover_id == b->cover_id);
  source_free (source);
  g_clear_object (&old);
  assert_metadata (replacement);
}

static void test_optional_fields (void)
{
  SpotifyNativeTrack source = { .uri = "local:track:fixture" };
  g_autoptr(SpotifyGtkTrackItem) item = spotifygtk_track_item_new (&source, 0);
  const SpotifyNativeTrack *track = spotifygtk_track_item_get_track (item);
  g_assert_cmpstr (track->uri, ==, source.uri);
  g_assert_null (track->name);
  g_assert_null (track->cover_id);
  g_assert_null (track->section_title);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/track-item/source-released", test_source_released);
  g_test_add_func ("/track-item/retired-focus-item", test_retired_focus_item);
  g_test_add_func ("/track-item/replacement-shares-strings", test_replacement_shares_strings);
  g_test_add_func ("/track-item/optional-fields", test_optional_fields);
  return g_test_run ();
}

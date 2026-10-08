#include <adwaita.h>
#include "../src/ui/album_grid.c"

gchar *spotifygtk_context_share_url (const gchar *uri)
{
  (void) uri;
  return NULL;
}

void spotifygtk_cover_load_deferrable (const gchar *id, gint pixels,
  GCancellable *cancel, SpotifyCoverCallback callback, gpointer data)
{
  (void) id; (void) pixels; (void) cancel;
  callback (NULL, data);
}
static void test_artist_and_playlist_pending (void)
{
  g_autoptr(SpotifyGtkAlbumGrid) grid = g_object_ref_sink (spotifygtk_album_grid_new_grid ());
  SpotifyGtkCardSpec cards[] = {
    { .uri = "spotify:artist:fixture", .title = "Artist" },
    { .uri = "spotify:playlist:fixture", .title = "Playlist" },
    { .uri = "spotify:album:fixture", .title = "Album" },
    { .uri = "local:playlist:fixture", .title = "Device playlist" },
  };
  spotifygtk_album_grid_set_pending_cards (grid, cards, G_N_ELEMENTS (cards));
  for (guint i = 0; i < G_N_ELEMENTS (cards); i++) {
    g_autoptr(SpotifyGtkAlbumItem) item = g_list_model_get_item (G_LIST_MODEL (grid->store), i);
    g_assert_cmpint (item->pending, ==, i < 2);
  }
  spotifygtk_album_grid_resolve_card (grid, cards[0].uri, "Resolved artist", "Artist", "portrait");
  g_autoptr(SpotifyGtkAlbumItem) artist = g_list_model_get_item (G_LIST_MODEL (grid->store), 0);
  g_assert_false (artist->pending);
  g_assert_cmpstr (artist->name, ==, "Resolved artist");
  g_assert_cmpstr (artist->cover_id, ==, "portrait");
}
int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ()) return 77;
#if GTK_CHECK_VERSION(4, 24, 0)
  g_object_set (gtk_settings_get_default (), "gtk-interface-color-scheme",
                 GTK_INTERFACE_COLOR_SCHEME_DEFAULT, NULL);
#else
  g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", FALSE, NULL);
#endif
  adw_init ();
  g_test_add_func ("/cards/artist-playlist-pending", test_artist_and_playlist_pending);
  return g_test_run ();
}

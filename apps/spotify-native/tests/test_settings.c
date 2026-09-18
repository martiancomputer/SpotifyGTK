#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "ui/settings.h"

#ifdef SPOTIFYGTK_UI_TESTS
#include "ui/context_page.h"
#include "ui/search_page.h"
#include "ui/artist_page.h"
#include "ui/track_item.h"
#include "ui/track_row.h"

void spotifygtk_context_page_test_complete (SpotifyGtkContextPage *self,
  const gchar *uri, GPtrArray *tracks);
void spotifygtk_search_page_test_playlists (SpotifyGtkSearchPage *self,
  const gchar *json, const gchar *query);
void spotifygtk_search_page_test_complete (SpotifyGtkSearchPage *self,
  GPtrArray *tracks, const gchar *query);

static gboolean
contains_label (GtkWidget *widget, const gchar *text)
{
  if (GTK_IS_LABEL (widget) && strstr (gtk_label_get_text (GTK_LABEL (widget)), text)) return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    if (contains_label (child, text)) return TRUE;
  return FALSE;
}

static void
settle (guint milliseconds)
{
  gint64 end = g_get_monotonic_time () + milliseconds * 1000;
  do {
    while (g_main_context_iteration (NULL, FALSE)) {}
    g_usleep (1000);
  } while (g_get_monotonic_time () < end);
}

static GtkWidget *
find_widget (GtkWidget *root, GType type, const gchar *css)
{
  if (g_type_is_a (G_OBJECT_TYPE (root), type) &&
      (!css || gtk_widget_has_css_class (root, css))) return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *found = find_widget (child, type, css);
    if (found) return found;
  }
  return NULL;
}

static void
capture (GtkWindow *window, const gchar *name)
{
  const gchar *directory = g_getenv ("SPOTIFYGTK_UI_CAPTURE_DIRECTORY");
  if (!directory) return;
  GtkWidget *widget = GTK_WIDGET (window);
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new (widget);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot),
                         gtk_widget_get_width (widget), gtk_widget_get_height (widget));
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  graphene_rect_t viewport = GRAPHENE_RECT_INIT (0, 0,
    gtk_widget_get_width (widget), gtk_widget_get_height (widget));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture (
    gtk_native_get_renderer (GTK_NATIVE (window)), node, &viewport);
  g_autofree gchar *path = g_build_filename (directory, name, NULL);
  g_assert_true (gdk_texture_save_to_png (texture, path));
  g_test_message ("UI capture: %s", path);
}

static void
assert_frames_settle (GtkWidget *widget)
{
  settle (700);
  GdkFrameClock *clock = gtk_widget_get_frame_clock (widget);
  gint64 before = gdk_frame_clock_get_frame_counter (clock);
  settle (200);
  g_assert_cmpint (gdk_frame_clock_get_frame_counter (clock) - before, <=, 2);
}

static void
count_context_activation (SpotifyGtkAlbumGrid *grid, const gchar *uri,
                          const gchar *name, guint *count)
{
  g_assert_true (g_str_has_prefix (uri, "spotify:album:") ||
                 g_str_has_prefix (uri, "spotify:playlist:"));
  g_assert_nonnull (name);
  (*count)++;
  (void) grid;
}

static void
assert_song_item_retained (GtkSelectionModel *model, gpointer song)
{
  g_autoptr(GObject) first = g_list_model_get_item (G_LIST_MODEL (model), 0);
  g_assert_true (first == song);
}

typedef struct {
  guint position, removed, added, count;
} ModelChange;

static void
record_model_change (GListModel *model, guint position, guint removed, guint added,
                      ModelChange *change)
{
  *change = (ModelChange) { position, removed, added, change->count + 1 };
  (void) model;
}

static void
test_presentation (void)
{
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  spotifygtk_settings_set_compact_mode (settings, TRUE);
  GtkWindow *window = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_default_size (window, 820, 680);
  SpotifyGtkContextPage *context = spotifygtk_context_page_new ();
  gtk_window_set_child (window, GTK_WIDGET (context));
  spotifygtk_context_page_load (context, "spotify:album:1234567890123456789012",
    "We Are Friends, Vol. 10", "Album");
  spotifygtk_context_page_set_action (context, "Save", TRUE, FALSE);
  g_autoptr(GPtrArray) tracks = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_track_free);
  for (guint i = 0; i < 300; i++) {
    SpotifyNativeTrack *track = g_new0 (SpotifyNativeTrack, 1);
    track->uri = g_strdup_printf ("spotify:track:%022u", i);
    track->name = g_strdup_printf ("Track %u", i + 1);
    track->artists = g_strdup (i ? "Artist Two" : "Artist One");
    track->album = g_strdup ("We Are Friends, Vol. 10");
    track->album_uri = g_strdup (i < 150 ? "spotify:album:1234567890123456789012"
                                        : "spotify:album:2234567890123456789012");
    track->duration_ms = 180000;
    track->release_year = 2021;
    g_ptr_array_add (tracks, track);
  }
  SpotifyGtkTrackList *list = spotifygtk_context_page_get_list (context);
  spotifygtk_context_page_test_complete (context, "spotify:album:1234567890123456789012", tracks);
  g_assert_true (contains_label (GTK_WIDGET (context), "2021 · Multiple artists · 300 tracks · 15 hr 0 min"));
  gtk_window_present (window);
  settle (400);
  GtkWidget *view = find_widget (GTK_WIDGET (list), GTK_TYPE_LIST_VIEW, NULL);
  g_assert_nonnull (view);
  GtkSelectionModel *model = gtk_list_view_get_model (GTK_LIST_VIEW (view));
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (model)), ==, 300);
  capture (window, "context-compact.png");
  GtkWidget *context_scroller = gtk_widget_get_first_child (GTK_WIDGET (context));
  g_assert_true (GTK_IS_SCROLLED_WINDOW (context_scroller));
  GtkAdjustment *context_adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (context_scroller));
  GtkWidget *compact_title = find_widget (GTK_WIDGET (context), GTK_TYPE_LABEL, "title-text");
  graphene_rect_t context_before, context_after;
  g_assert_true (gtk_widget_compute_bounds (compact_title, GTK_WIDGET (context), &context_before));
  gtk_adjustment_set_value (context_adjustment, 180);
  settle (150);
  g_assert_true (gtk_widget_compute_bounds (compact_title, GTK_WIDGET (context), &context_after));
  g_assert_cmpfloat_with_epsilon (context_before.origin.y - context_after.origin.y, 180, 1);
  gtk_adjustment_set_value (context_adjustment, 0);
  spotifygtk_settings_set_compact_mode (settings, FALSE);
  settle (300);
  g_assert_true (model == gtk_list_view_get_model (GTK_LIST_VIEW (view)));
  GtkWidget *title = find_widget (GTK_WIDGET (context), GTK_TYPE_LABEL, "context-hero-title");
  g_assert_true (gtk_widget_get_mapped (title));
  GtkWidget *picture = find_widget (GTK_WIDGET (context), GTK_TYPE_PICTURE, NULL);
  g_assert_cmpint (gtk_picture_get_content_fit (GTK_PICTURE (picture)), ==, GTK_CONTENT_FIT_COVER);
  g_assert_cmpint (gtk_widget_get_width (picture), ==, 256);
  g_assert_cmpint (gtk_widget_get_height (picture), ==, 256);
  capture (window, "context-expanded.png");
  assert_frames_settle (GTK_WIDGET (context));
  /* The entire hero/divider leaves the page viewport, not just its tracks. */
  GtkWidget *hero_header = gtk_widget_get_parent (gtk_widget_get_parent (gtk_widget_get_parent (title)));
  g_assert_true (gtk_widget_compute_bounds (hero_header, GTK_WIDGET (context), &context_before));
  guint8 header_pixels[3] = { 48, 64, 80 };
  g_autoptr(GBytes) header_bytes = g_bytes_new (header_pixels, sizeof header_pixels);
  g_autoptr(GdkTexture) header_texture = GDK_TEXTURE (
    gdk_memory_texture_new (1, 1, GDK_MEMORY_R8G8B8, header_bytes, 3));
  gtk_picture_set_paintable (GTK_PICTURE (picture), GDK_PAINTABLE (header_texture));
  gtk_adjustment_set_value (context_adjustment, 500);
  settle (150);
  g_assert_true (gtk_widget_compute_bounds (hero_header, GTK_WIDGET (context), &context_after));
  g_assert_cmpfloat_with_epsilon (context_before.origin.y - context_after.origin.y, 500, 1);
  g_assert_cmpfloat (context_after.origin.y + context_after.size.height, <, 0);
  g_assert_null (gtk_picture_get_paintable (GTK_PICTURE (picture)));
  capture (window, "context-expanded-scrolled.png");
  gtk_adjustment_set_value (context_adjustment, 0);
  gtk_window_set_default_size (window, 450, 680);
  gtk_label_set_text (GTK_LABEL (title), "A very long album title which must wrap and never impose an enormous minimum window width");
  settle (300);
  g_assert_cmpint (gtk_widget_get_width (GTK_WIDGET (window)), <=, 450);
  capture (window, "context-narrow.png");
  gtk_adjustment_set_value (context_adjustment, 500);
  spotifygtk_context_page_load (context, "spotify:playlist:1234567890123456789012", "Friends Playlist", "Playlist");
  g_assert_cmpfloat (gtk_adjustment_get_value (context_adjustment), ==, 0);
  spotifygtk_context_page_test_complete (context, "spotify:playlist:1234567890123456789012", tracks);
  g_assert_true (contains_label (GTK_WIDGET (context), "Multiple artists · 300 tracks · 15 hr 0 min"));
  g_assert_false (contains_label (GTK_WIDGET (context), "2021"));
  settle (150);
  gtk_adjustment_set_value (context_adjustment, 500);
  settle (150);
  g_assert_true (gtk_widget_compute_bounds (hero_header, GTK_WIDGET (context), &context_after));
  g_assert_cmpfloat (context_after.origin.y + context_after.size.height, <, 0);
  spotifygtk_context_page_load (context, "spotify:playlist:1234567890123456789012", "Friends Playlist", "Playlist");
  g_assert_cmpfloat (gtk_adjustment_get_value (context_adjustment), ==, 500);
  gtk_window_destroy (window);

  window = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_default_size (window, 820, 680);
  SpotifyGtkSearchPage *search = spotifygtk_search_page_new ();
  spotifygtk_settings_set_aggressive_filtering (settings, FALSE);
  spotifygtk_search_page_test_complete (search, tracks, "friends");
  spotifygtk_search_page_test_playlists (search,
    "{\"data\":{\"searchV2\":{\"playlists\":{\"items\":["
    "{\"data\":{\"uri\":\"spotify:playlist:1234567890123456789012\",\"name\":\"Friends Mix\"}},"
    "{\"item\":{\"data\":{\"uri\":\"spotify:playlist:2234567890123456789012\",\"name\":\"Chill\",\"ownerV2\":{\"data\":{\"name\":\"Friends Collector\"}}}}},"
    "{\"data\":{\"uri\":\"spotify:playlist:3234567890123456789012\",\"name\":\"Unrelated\"}}"
    "]}}}}", "friends");
  GtkWidget *stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (stack), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_stack_add_named (GTK_STACK (stack), GTK_WIDGET (search), "search");
  gtk_stack_add_named (GTK_STACK (stack), gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), "album");
  gtk_window_set_child (window, stack);
  SpotifyGtkAlbumGrid *albums = spotifygtk_search_page_get_album_grid (search);
  gtk_window_present (window);
  settle (300);
  GtkWidget *grid = find_widget (GTK_WIDGET (albums), GTK_TYPE_LIST_VIEW, NULL);
  g_assert_nonnull (grid);
  model = gtk_list_view_get_model (GTK_LIST_VIEW (grid));
  GtkWidget *playlist_view = find_widget (GTK_WIDGET (spotifygtk_search_page_get_playlist_grid (search)), GTK_TYPE_LIST_VIEW, NULL);
  GtkSelectionModel *playlist_model = gtk_list_view_get_model (GTK_LIST_VIEW (playlist_view));
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (playlist_model)), ==, 3);
  g_assert_true (contains_label (GTK_WIDGET (albums), "Album · Artist One"));
  g_assert_true (contains_label (GTK_WIDGET (search), "Track"));
  spotifygtk_settings_set_aggressive_filtering (settings, TRUE);
  settle (100);
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (playlist_model)), ==, 2);
  spotifygtk_settings_set_aggressive_filtering (settings, FALSE);
  settle (100);
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (playlist_model)), ==, 3);
  spotifygtk_settings_set_compact_mode (settings, TRUE);
  settle (300);
  g_assert_true (model == gtk_list_view_get_model (GTK_LIST_VIEW (grid)));
  g_assert_false (gtk_widget_get_visible (gtk_widget_get_parent (GTK_WIDGET (albums))));
  g_assert_false (gtk_widget_get_visible (gtk_widget_get_parent (
    GTK_WIDGET (spotifygtk_search_page_get_playlist_grid (search)))));
  SpotifyGtkTrackList *results = spotifygtk_search_page_get_list (search);
  GtkWidget *result_view = find_widget (GTK_WIDGET (results), GTK_TYPE_LIST_VIEW, NULL);
  GtkSelectionModel *result_model = gtk_list_view_get_model (GTK_LIST_VIEW (result_view));
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (result_model)), ==, 305);
  g_autoptr(GObject) first_song = g_list_model_get_item (G_LIST_MODEL (result_model), 0);
  g_autoptr(SpotifyGtkTrackItem) album_item = g_list_model_get_item (G_LIST_MODEL (result_model), 1);
  g_assert_cmpstr (spotifygtk_track_item_get_track (album_item)->uri, ==,
                  "spotify:album:1234567890123456789012");
  g_autoptr(GPtrArray) playable = spotifygtk_track_list_snapshot (results);
  g_assert_cmpuint (playable->len, ==, 300);
  guint activations = 0;
  g_signal_connect (albums, "album-activated", G_CALLBACK (count_context_activation), &activations);
  g_signal_emit_by_name (result_view, "activate", 1u);
  g_assert_cmpuint (activations, ==, 1);
  g_assert_true (contains_label (GTK_WIDGET (results), "Album"));
  GtkWidget *menu_anchor = find_widget (GTK_WIDGET (results), SPOTIFYGTK_TYPE_TRACK_ROW, NULL);
  g_signal_emit_by_name (results, "context-menu", spotifygtk_track_item_get_track (album_item),
                         menu_anchor, 16.0, 16.0);
  settle (100);
  g_assert_true (contains_label (menu_anchor, "Share Album"));
  GtkWidget *menu = find_widget (menu_anchor, GTK_TYPE_POPOVER, NULL);
  g_assert_nonnull (menu);
  gtk_popover_popdown (GTK_POPOVER (menu));
  settle (200);
  g_assert_null (find_widget (menu_anchor, GTK_TYPE_POPOVER, NULL));
  guint playlist_activations = 0;
  g_signal_connect (spotifygtk_search_page_get_playlist_grid (search), "album-activated",
                     G_CALLBACK (count_context_activation), &playlist_activations);
  g_signal_emit_by_name (result_view, "activate", 304u);
  g_assert_cmpuint (playlist_activations, ==, 1);
  g_autoptr(SpotifyGtkTrackItem) playlist_item = g_list_model_get_item (G_LIST_MODEL (result_model), 304);
  g_signal_emit_by_name (results, "context-menu", spotifygtk_track_item_get_track (playlist_item),
                         menu_anchor, 16.0, 16.0);
  settle (100);
  g_assert_true (contains_label (menu_anchor, "Share Playlist"));
  menu = find_widget (menu_anchor, GTK_TYPE_POPOVER, NULL);
  g_assert_nonnull (menu);
  gtk_popover_popdown (GTK_POPOVER (menu));
  settle (200);
  capture (window, "search-compact.png");

  /* A late playlist answer is a suffix-only splice, not a refresh of tracks. */
  ModelChange change = { 0 };
  g_signal_connect (result_model, "items-changed", G_CALLBACK (record_model_change), &change);
  spotifygtk_settings_set_aggressive_filtering (settings, TRUE);
  settle (100);
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (result_model)), ==, 304);
  assert_song_item_retained (result_model, first_song);
  g_assert_cmpuint (change.count, ==, 1);
  g_assert_cmpuint (change.position, >=, 302);
  g_assert_cmpuint (change.removed, ==, 1);
  g_assert_cmpuint (change.added, ==, 0);
  spotifygtk_settings_set_aggressive_filtering (settings, FALSE);
  settle (100);
  assert_song_item_retained (result_model, first_song);

  /* Reproduce search -> album -> search with navigation's artwork lifecycle. */
  spotifygtk_track_list_release_covers (results);
  spotifygtk_album_grid_release_covers (albums);
  gtk_stack_set_visible_child_name (GTK_STACK (stack), "album");
  settle (100);
  gtk_stack_set_visible_child_name (GTK_STACK (stack), "search");
  spotifygtk_track_list_reload_covers (results);
  spotifygtk_album_grid_reload_covers (albums);
  settle (200);
  assert_song_item_retained (result_model, first_song);
  g_assert_false (gtk_widget_get_mapped (GTK_WIDGET (albums)));
  GtkWidget *row = find_widget (GTK_WIDGET (results), SPOTIFYGTK_TYPE_TRACK_ROW, NULL);
  GtkWidget *row_art = find_widget (row, GTK_TYPE_IMAGE, "card");
  g_assert_cmpint (gtk_image_get_pixel_size (GTK_IMAGE (row_art)), ==, 40);
  capture (window, "search-compact-return.png");

  GtkWidget *outer = gtk_widget_get_first_child (GTK_WIDGET (search));
  g_assert_true (GTK_IS_SCROLLED_WINDOW (outer));
  GtkWidget *entry = find_widget (GTK_WIDGET (search), GTK_TYPE_SEARCH_ENTRY, NULL);
  GtkWidget *header = gtk_widget_get_parent (entry);
  graphene_rect_t before, after;
  g_assert_true (gtk_widget_compute_bounds (header, GTK_WIDGET (search), &before));
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (outer));
  gtk_adjustment_set_value (adjustment, 180);
  settle (200);
  g_assert_true (gtk_widget_compute_bounds (header, GTK_WIDGET (search), &after));
  g_assert_cmpfloat_with_epsilon (before.origin.y - after.origin.y, 180, 1);
  g_assert_cmpfloat (after.origin.y + after.size.height, <, 0);
  capture (window, "search-compact-scrolled.png");
  gtk_adjustment_set_value (adjustment, 0);
  spotifygtk_settings_set_compact_mode (settings, FALSE);
  settle (300);
  g_assert_true (model == gtk_list_view_get_model (GTK_LIST_VIEW (grid)));
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (result_model)), ==, 300);
  assert_song_item_retained (result_model, first_song);
  g_assert_true (gtk_widget_get_mapped (GTK_WIDGET (albums)));
  capture (window, "search-expanded.png");
  gtk_adjustment_set_value (adjustment, 400);
  settle (200);
  g_assert_true (gtk_widget_compute_bounds (header, GTK_WIDGET (search), &after));
  g_assert_cmpfloat (after.origin.y + after.size.height, <, 0);
  capture (window, "search-expanded-scrolled.png");
  gtk_window_destroy (window);

  /* Exercise the grid's compact release path with an actual paintable. */
  window = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_default_size (window, 820, 180);
  albums = spotifygtk_album_grid_new_shelf ();
  spotifygtk_album_grid_set_compact (albums, TRUE);
  SpotifyGtkCardSpec card_spec = { "spotify:album:1234567890123456789012", "Cover release", "Artist", NULL };
  spotifygtk_album_grid_set_cards (albums, &card_spec, 1);
  gtk_window_set_child (window, GTK_WIDGET (albums));
  gtk_window_present (window);
  settle (150);
  GtkWidget *card = find_widget (GTK_WIDGET (albums), GTK_TYPE_BUTTON, "media-card");
  g_assert_nonnull (card);
  GtkImage *art = g_object_get_data (G_OBJECT (card), "art");
  guint8 pixels[3] = { 32, 64, 96 };
  g_autoptr(GBytes) bytes = g_bytes_new (pixels, sizeof pixels);
  g_autoptr(GdkTexture) texture = GDK_TEXTURE (gdk_memory_texture_new (1, 1, GDK_MEMORY_R8G8B8, bytes, 3));
  gtk_image_set_from_paintable (art, GDK_PAINTABLE (texture));
  g_object_set_data (G_OBJECT (card), "cover-shown", GUINT_TO_POINTER (1));
  spotifygtk_album_grid_release_covers (albums);
  g_assert_cmpint (gtk_image_get_pixel_size (art), ==, 48);
  gtk_window_destroy (window);

  window = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_default_size (window, 450, 680);
  gtk_window_set_child (window, GTK_WIDGET (spotifygtk_artist_page_new ()));
  gtk_window_present (window);
  assert_frames_settle (GTK_WIDGET (window));
  gtk_window_destroy (window);
  settle (100);
}

int
main (int argc, char **argv)
{
  /* Skip on ordinary headless CI; xvfb-run exercises the real widget paths. */
  if (!g_getenv ("DISPLAY") && !g_getenv ("WAYLAND_DISPLAY")) return 77;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *temporary = g_dir_make_tmp ("spotifygtk-ui-settings-XXXXXX", &error);
  g_assert_no_error (error);
  g_setenv ("XDG_CONFIG_HOME", temporary, TRUE);
  g_test_init (&argc, &argv, NULL);
  adw_init ();
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (css,
    ".context-hero-title { font-size:40px; font-weight:800; }"
    ".title-text { font-size:30px; font-weight:800; }"
    ".art-large { border-radius:12px; background:#ddd; }"
    ".track-row { min-height:56px; }");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
    GTK_STYLE_PROVIDER (css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);
  g_test_add_func ("/ui/presentation/modes-models-geometry-and-idle", test_presentation);
  gint result = g_test_run ();
  g_autofree gchar *config = g_build_filename (temporary, "spotify-native", "settings.ini", NULL);
  g_autofree gchar *directory = g_build_filename (temporary, "spotify-native", NULL);
  g_remove (config); g_rmdir (directory); g_rmdir (temporary);
  return result;
}
#else

static void
test_renderer_backend (void)
{
  g_assert_null (spotifygtk_renderer_backend (SPOTIFYGTK_RENDERER_AUTOMATIC));
  g_assert_cmpstr (spotifygtk_renderer_backend (SPOTIFYGTK_RENDERER_VULKAN),
                   ==, "vulkan");
  g_assert_cmpstr (spotifygtk_renderer_backend (SPOTIFYGTK_RENDERER_OPENGL),
                   ==, "opengl");
  g_assert_cmpstr (spotifygtk_renderer_backend (SPOTIFYGTK_RENDERER_CAIRO),
                   ==, "cairo");
  g_assert_null (spotifygtk_renderer_backend ((SpotifyGtkRenderer) 99));
}

static void
test_online_lyrics_opt_in (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *temporary = g_dir_make_tmp (
    "spotifygtk-lyrics-settings-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (temporary);
  g_setenv ("XDG_CONFIG_HOME", temporary, TRUE);

  g_autoptr(SpotifyGtkSettings) first =
    g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  g_assert_false (spotifygtk_settings_get_online_lyrics (first));
  g_assert_true (spotifygtk_settings_get_compact_mode (first));
  spotifygtk_settings_set_compact_mode (first, FALSE);
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (first), ==, 19);
  spotifygtk_settings_set_lyrics_font_size (first, 28);
  spotifygtk_settings_set_lyrics_font_size (first, 29);
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (first), ==, 28);
  spotifygtk_settings_set_online_lyrics (first, TRUE);
  g_assert_true (spotifygtk_settings_get_online_lyrics (first));

  g_autoptr(SpotifyGtkSettings) second =
    g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  g_assert_true (spotifygtk_settings_get_online_lyrics (second));
  g_assert_false (spotifygtk_settings_get_compact_mode (second));
  spotifygtk_settings_set_compact_mode (second, TRUE);
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (second), ==, 28);
  spotifygtk_settings_set_lyrics_font_size (second, 19);
  spotifygtk_settings_set_online_lyrics (second, FALSE);
  g_autoptr(SpotifyGtkSettings) third =
    g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  g_assert_false (spotifygtk_settings_get_online_lyrics (third));
  g_assert_true (spotifygtk_settings_get_compact_mode (third));
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (third), ==, 19);

  g_autofree gchar *config = g_build_filename (
    temporary, "spotify-native", "settings.ini", NULL);
  g_autofree gchar *config_dir = g_build_filename (
    temporary, "spotify-native", NULL);
  g_assert_cmpint (g_remove (config), ==, 0);
  g_assert_cmpint (g_rmdir (config_dir), ==, 0);
  g_assert_cmpint (g_rmdir (temporary), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/settings/renderer-backend", test_renderer_backend);
  g_test_add_func ("/settings/online-lyrics-opt-in", test_online_lyrics_opt_in);
  return g_test_run ();
}
#endif

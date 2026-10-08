#include "../src/ui/track_row.c"

/* Pointer routing does not need artwork or network access. */
void
spotifygtk_cover_load_deferrable (const gchar *id, gint pixels,
                                 GCancellable *cancel,
                                 SpotifyCoverCallback callback, gpointer data)
{
  (void) id; (void) pixels; (void) cancel;
  callback (NULL, data);
}

static GtkWidget *
pick_center (GtkWidget *root, GtkWidget *child)
{
  graphene_rect_t bounds;
  g_assert_true (gtk_widget_compute_bounds (child, root, &bounds));
  return gtk_widget_pick (root,
                          bounds.origin.x + bounds.size.width / 2,
                          bounds.origin.y + bounds.size.height / 2,
                          GTK_PICK_DEFAULT);
}

static void
play_clicked (SpotifyGtkTrackRow *row, gpointer data)
{
  (*(guint *) data)++;
  (void) row;
}

static void
test_pointer_routing (void)
{
  GtkWidget *window = gtk_window_new ();
  SpotifyGtkTrackRow *row = spotifygtk_track_row_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 800, 80);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (row));
  gtk_window_present (GTK_WINDOW (window));
  gint64 deadline = g_get_monotonic_time () + G_USEC_PER_SEC;
  while (gtk_widget_get_width (GTK_WIDGET (row)) == 0 &&
         g_get_monotonic_time () < deadline) {
    g_main_context_iteration (NULL, FALSE);
    g_usleep (1000);
  }
  g_assert_cmpint (gtk_widget_get_width (GTK_WIDGET (row)), >, 0);

  GtkWidget *info = gtk_widget_get_parent (GTK_WIDGET (row->title_label));
  g_assert_false (gtk_widget_get_can_target (info));
  g_assert_false (gtk_widget_get_can_target (row->status_slot));
  GtkWidget *picked = pick_center (GTK_WIDGET (window), info);
  g_assert_nonnull (picked);
  g_assert_false (picked == info || gtk_widget_is_ancestor (picked, info));
  g_assert_true (picked == GTK_WIDGET (row) ||
                 gtk_widget_is_ancestor (picked, GTK_WIDGET (row)));

  on_row_hover_enter (NULL, 0, 0, row);
  picked = pick_center (GTK_WIDGET (window), GTK_WIDGET (row->play_btn));
  g_assert_true (picked == GTK_WIDGET (row->play_btn) ||
                 gtk_widget_is_ancestor (picked, GTK_WIDGET (row->play_btn)));
  guint clicks = 0;
  g_signal_connect (row, "play-clicked", G_CALLBACK (play_clicked), &clicks);
  g_signal_emit_by_name (row->play_btn, "clicked");
  g_assert_cmpuint (clicks, ==, 1);
  on_row_hover_leave (NULL, row);
  picked = pick_center (GTK_WIDGET (window), GTK_WIDGET (row->play_btn));
  g_assert_false (picked == GTK_WIDGET (row->play_btn) ||
                  gtk_widget_is_ancestor (picked, GTK_WIDGET (row->play_btn)));
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
setup_activation_item (GtkListItemFactory *factory, GtkListItem *item,
                       gpointer data)
{
  gtk_list_item_set_child (item, gtk_label_new ("Playable row"));
  gtk_list_item_set_selectable (item, FALSE);
  gtk_list_item_set_activatable (item, TRUE);
  gtk_list_item_set_focusable (item, TRUE);
  *(GtkListItem **) data = item;
  (void) factory;
}

static void
list_activated (GtkListView *view, guint position, gpointer data)
{
  g_assert_cmpuint (position, ==, 0);
  (*(guint *) data)++;
  (void) view;
}

static void
test_activation_without_selection (void)
{
  const gchar *strings[] = { "Track", NULL };
  GtkListItem *item = NULL;
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (setup_activation_item), &item);
  GtkSelectionModel *model = GTK_SELECTION_MODEL (gtk_no_selection_new (
    G_LIST_MODEL (gtk_string_list_new (strings))));
  GtkWidget *view = gtk_list_view_new (model, factory);
  gtk_list_view_set_single_click_activate (GTK_LIST_VIEW (view), TRUE);
  guint activations = 0;
  g_signal_connect (view, "activate", G_CALLBACK (list_activated), &activations);
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_child (GTK_WINDOW (window), view);
  gtk_window_present (GTK_WINDOW (window));
  gint64 deadline = g_get_monotonic_time () + G_USEC_PER_SEC;
  while ((!item || !gtk_widget_get_mapped (view)) &&
         g_get_monotonic_time () < deadline) {
    g_main_context_iteration (NULL, FALSE);
    g_usleep (1000);
  }
  g_assert_nonnull (item);
  GtkWidget *wrapper = gtk_widget_get_parent (gtk_list_item_get_child (item));
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers (wrapper);
  gboolean released = FALSE;
  for (guint i = 0; i < g_list_model_get_n_items (controllers); i++) {
    g_autoptr(GObject) controller = g_list_model_get_item (controllers, i);
    if (GTK_IS_GESTURE_CLICK (controller)) {
      /* Exercise GTK's single-click activation handler with selection
       * disabled, rather than just emitting ListView::activate ourselves. */
      g_signal_emit_by_name (controller, "released", 1, 1.0, 1.0);
      released = TRUE;
    }
  }
  g_assert_true (released);
  g_assert_cmpuint (activations, ==, 1);
  g_assert_false (gtk_selection_model_is_selected (model, 0));
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_metadata_width (void)
{
  g_autoptr(SpotifyGtkTrackRow) row = g_object_ref_sink (spotifygtk_track_row_new ());
  SpotifyNativeTrack track = {
    .uri = "spotify:track:test",
    .name = "A long soundtrack title that should fit comfortably in a wide row",
    .artists = "Composer One, Composer Two, Orchestra Three",
    .album = "An Original Soundtrack Selection with a Long Album Name",
  };
  spotifygtk_track_row_set_native_track (row, &track, 1);
  gtk_widget_allocate (GTK_WIDGET (row), 1200, 80, -1, NULL);
  g_assert_cmpint (gtk_widget_get_width (GTK_WIDGET (row->metadata_label)), >, 1000);
  g_assert_false (pango_layout_is_ellipsized (gtk_label_get_layout (row->metadata_label)));
  g_assert_false (pango_layout_is_ellipsized (gtk_label_get_layout (row->title_label)));

  gtk_widget_allocate (GTK_WIDGET (row), 350, 80, -1, NULL);
  g_assert_true (pango_layout_is_ellipsized (gtk_label_get_layout (row->metadata_label)));
  g_assert_true (pango_layout_is_ellipsized (gtk_label_get_layout (row->title_label)));

  g_autofree gchar *long_text = g_strnfill (5000, 'W');
  track.artists = long_text;
  track.album = long_text;
  spotifygtk_track_row_set_native_track (row, &track, 1);
  int minimum, natural;
  gtk_widget_measure (GTK_WIDGET (row), GTK_ORIENTATION_HORIZONTAL, -1,
                      &minimum, &natural, NULL, NULL);
  g_assert_cmpint (minimum, <, 350);
  g_assert_cmpint (natural, <, 1000);
}

static void
test_metadata_visibility_and_reuse (void)
{
  g_autoptr(SpotifyGtkTrackRow) row = g_object_ref_sink (spotifygtk_track_row_new ());
  SpotifyNativeTrack track = {
    .uri = "local:track:test", .name = "Track", .artists = "Artist", .album = "Album"
  };
  spotifygtk_track_row_set_native_track (row, &track, 1);
  g_assert_cmpstr (gtk_label_get_text (row->metadata_label), ==, "Artist · On this device  Album");
  spotifygtk_track_row_set_show_album (row, FALSE);
  g_assert_cmpstr (gtk_label_get_text (row->metadata_label), ==, "Artist · On this device");
  spotifygtk_track_row_set_show_artists (row, FALSE);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (row->metadata_label)));
  spotifygtk_track_row_set_show_album (row, TRUE);
  g_assert_cmpstr (gtk_label_get_text (row->metadata_label), ==, "Album");
  spotifygtk_track_row_set_show_artists (row, TRUE);
  track.uri = "spotify:album:test";
  spotifygtk_track_row_set_native_track (row, &track, 1);
  spotifygtk_track_row_set_show_album (row, TRUE);
  g_assert_cmpstr (gtk_label_get_text (row->metadata_label), ==, "Artist");
  g_autoptr(JsonObject) empty = json_object_new ();
  spotifygtk_track_row_set_track (row, empty, 1);
  g_assert_cmpstr (gtk_label_get_text (row->metadata_label), ==, "");
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (row->metadata_label)));
}

static void
test_equalizer_centered_on_duration (void)
{
  g_autoptr(SpotifyGtkTrackRow) row = g_object_ref_sink (spotifygtk_track_row_new ());
  const gchar *durations[] = { "3:28", "10:24", "0:59" };
  for (guint i = 0; i < G_N_ELEMENTS (durations); i++) {
    gtk_label_set_text (row->duration_label, durations[i]);
    gint text_width = 0;
    pango_layout_get_pixel_size (gtk_label_get_layout (row->duration_label),
                                 &text_width, NULL);
    gint width = MAX (text_width, 44);
    cairo_surface_t *surface = cairo_image_surface_create (
      CAIRO_FORMAT_ARGB32, width, EQ_HEIGHT);
    cairo_t *cr = cairo_create (surface);
    eq_draw (GTK_DRAWING_AREA (row->eq_area), cr, width, EQ_HEIGHT, row);
    cairo_surface_flush (surface);
    const guint32 *pixels = (const guint32 *) cairo_image_surface_get_data (surface);
    gint stride = cairo_image_surface_get_stride (surface) / 4;
    gint left = width, right = -1;
    for (gint y = 0; y < EQ_HEIGHT; y++)
      for (gint x = 0; x < width; x++)
        if (pixels[y * stride + x] >> 24) {
          left = MIN (left, x);
          right = MAX (right, x);
        }
    g_assert_cmpint (right, >=, left);
    g_assert_cmpfloat (fabs ((left + right + 1) / 2.0 -
                             (width - text_width / 2.0)), <=, 0.5);
    cairo_destroy (cr);
    cairo_surface_destroy (surface);
  }
  g_assert_cmpint (gtk_widget_get_halign (row->eq_area), ==, GTK_ALIGN_FILL);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    return 77;
  g_test_add_func ("/track-row/pointer-routing", test_pointer_routing);
  g_test_add_func ("/track-row/activation-without-selection",
                   test_activation_without_selection);
  g_test_add_func ("/track-row/metadata-width", test_metadata_width);
  g_test_add_func ("/track-row/metadata-visibility-reuse", test_metadata_visibility_and_reuse);
  g_test_add_func ("/track-row/equalizer-centered", test_equalizer_centered_on_duration);
  return g_test_run ();
}

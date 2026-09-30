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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    return 77;
  g_test_add_func ("/track-row/pointer-routing", test_pointer_routing);
  g_test_add_func ("/track-row/activation-without-selection",
                   test_activation_without_selection);
  return g_test_run ();
}

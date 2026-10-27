#include "../src/ui/smooth_scroll.c"

static void
repeated_attach (void)
{
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  g_object_ref_sink (scroller);
  spotifygtk_smooth_scroll_attach (scroller, GTK_ORIENTATION_VERTICAL);
  gpointer first = g_object_get_data (G_OBJECT (scroller), "spotifygtk-smooth-scroll");
  g_assert_nonnull (first);
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers (GTK_WIDGET (scroller));
  guint count = g_list_model_get_n_items (controllers);
  spotifygtk_smooth_scroll_attach (scroller, GTK_ORIENTATION_VERTICAL);
  g_assert_true (first == g_object_get_data (G_OBJECT (scroller), "spotifygtk-smooth-scroll"));
  g_assert_cmpuint (g_list_model_get_n_items (controllers), ==, count);
  g_clear_object (&controllers);
  g_object_unref (scroller);
}

static void
variable_row_anchors (void)
{
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  g_object_ref_sink (scroller);
  gtk_scrolled_window_set_child (scroller, gtk_list_view_new (NULL, NULL));
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (scroller);
  gtk_adjustment_configure (adj, 3000, 0, 60000, 1, 100, 792);
  spotifygtk_smooth_scroll_attach (scroller, GTK_ORIENTATION_VERTICAL);
  SmoothScroll *ss = g_object_get_data (G_OBJECT (scroller), "spotifygtk-smooth-scroll");
  ss->last_set = 3000; ss->target = 4000;
  gtk_adjustment_set_value (adj, 3219);
  g_assert_false (reconcile_scroll_anchor (ss, adj));
  spotifygtk_smooth_scroll_set_variable_rows (scroller);
  g_assert_true (reconcile_scroll_anchor (ss, adj));
  g_assert_cmpfloat (ss->target, ==, 4219);
  /* GTK can re-anchor a later frame without another upper notification. */
  gtk_adjustment_set_value (adj, 3000);
  g_assert_true (reconcile_scroll_anchor (ss, adj));
  g_assert_cmpfloat (ss->target, ==, 4000);
  gtk_adjustment_set_value (adj, 4500);
  g_assert_true (reconcile_scroll_anchor (ss, adj));
  g_assert_cmpfloat (ss->target, ==, 5500);
  gtk_adjustment_set_value (adj, 4719);
  ss->tick = gtk_widget_add_tick_callback (GTK_WIDGET (scroller), smooth_scroll_tick, ss, NULL);
  GtkEventController *wheel = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  gdouble step;
  scroll_character (&step, NULL);
  g_assert_true (on_scroll (GTK_EVENT_CONTROLLER_SCROLL (wheel), 0, 1, ss));
  g_assert_cmpfloat (ss->target, ==, 5500 + 219 + step);
  spotifygtk_smooth_scroll_cancel (scroller);
  g_object_unref (wheel);
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers (GTK_WIDGET (scroller));
  guint count = g_list_model_get_n_items (controllers);
  spotifygtk_smooth_scroll_set_variable_rows (scroller);
  g_assert_cmpuint (g_list_model_get_n_items (controllers), ==, count);
  ss->tick = gtk_widget_add_tick_callback (GTK_WIDGET (scroller), smooth_scroll_tick, ss, NULL);
  variable_rows_unmap (GTK_WIDGET (scroller), NULL);
  g_assert_cmpuint (ss->tick, ==, 0);
  g_assert_cmpfloat (ss->target, ==, gtk_adjustment_get_value (adj));
  ss->tick = gtk_widget_add_tick_callback (GTK_WIDGET (scroller), smooth_scroll_tick, ss, NULL);
  gtk_adjustment_set_page_size (adj, 850);
  g_assert_cmpuint (ss->tick, ==, 0);
  g_clear_object (&controllers);
  g_object_unref (scroller);
}

static void
pixel_coordinates (void)
{
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  g_object_ref_sink (scroller);
  SmoothScroll ss = { .scroller = scroller };
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (scroller);
  gtk_adjustment_configure (adj, 0, 0, 1000, 1, 100, 100);
  GtkWidget *list = gtk_list_view_new (NULL, NULL);
  gtk_scrolled_window_set_child (scroller, list);
  gtk_adjustment_configure (adj, 0, 0, 1000, 1, 100, 100);
  set_adjustment_value (&ss, adj, 123.7);
  g_assert_cmpfloat (gtk_adjustment_get_value (adj), ==, 124);
  set_adjustment_value (&ss, adj, 12.2);
  g_assert_cmpfloat (gtk_adjustment_get_value (adj), ==, 12);
  g_assert_false (ss.writing_adjustment);

  GtkWidget *grid = gtk_grid_view_new (NULL, NULL);
  gtk_scrolled_window_set_child (scroller, grid);
  gtk_adjustment_configure (adj, 0, 0, 1000, 1, 100, 100);
  set_adjustment_value (&ss, adj, 41.6);
  g_assert_cmpfloat (gtk_adjustment_get_value (adj), ==, 42);

  gtk_scrolled_window_set_child (scroller, gtk_label_new ("Ordinary content"));
  gtk_adjustment_configure (adj, 0, 0, 1000, 1, 100, 100);
  set_adjustment_value (&ss, adj, 41.6);
  g_assert_cmpfloat_with_epsilon (gtk_adjustment_get_value (adj), 41.6, 0.0001);
  g_object_unref (scroller);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    return 77;
  g_test_add_func ("/smooth-scroll/virtual-list-pixel-coordinates", pixel_coordinates);
  g_test_add_func ("/smooth-scroll/repeated-attach", repeated_attach);
  g_test_add_func ("/smooth-scroll/variable-row-anchors", variable_row_anchors);
  return g_test_run ();
}

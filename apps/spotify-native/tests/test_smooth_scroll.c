#include "../src/ui/smooth_scroll.c"

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
  return g_test_run ();
}

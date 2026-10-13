#include "ui/layout_policy.h"

static void test_measure_bounds (void)
{
  /* The exact warning from the live log: 205px available, 206px required. */
  g_assert_cmpint (spotifygtk_layout_measure_extent (205, 206), ==, 206);
  g_assert_cmpint (spotifygtk_layout_measure_extent (206, 206), ==, 206);
  g_assert_cmpint (spotifygtk_layout_measure_extent (500, 206), ==, 500);
  g_assert_cmpint (spotifygtk_layout_measure_extent (-1, 206), ==, -1);
  g_assert_cmpint (spotifygtk_layout_measure_extent (0, 206), ==, 206);
}

static void test_column_boundaries (void)
{
  g_assert_cmpuint (spotifygtk_layout_columns (205, 206), ==, 1);
  g_assert_cmpuint (spotifygtk_layout_columns (411, 206), ==, 1);
  g_assert_cmpuint (spotifygtk_layout_columns (412, 206), ==, 2);
  g_assert_cmpuint (spotifygtk_layout_columns (1029, 206), ==, 4);
  g_assert_cmpuint (spotifygtk_layout_columns (1030, 206), ==, 5);
  g_assert_cmpuint (spotifygtk_layout_columns (0, 206), ==, 1);
}

static void test_chrome_accounting (void)
{
  /* Explicit button requests exclude the margins and list-row CSS that GTK
   * adds afterward. Uneven slots must still sum to exactly one shelf page. */
  const gint minima[] = { 200, 206, 214, 218, 240, 300, 600 };
  for (guint m = 0; m < G_N_ELEMENTS (minima); m++) {
    gint minimum = minima[m];
    for (gint available = 1; available <= 3000; available++) {
      gint width = MAX (available, minimum);
      guint columns = spotifygtk_layout_columns (width, minimum);
      gint base = width / columns;
      guint remainder = width % columns;
      gint total = 0;
      for (guint i = 0; i < columns; i++) {
        gint slot = base + (i < remainder);
        gint request = spotifygtk_layout_card_request (slot, minimum, 200);
        g_assert_cmpint (slot, >=, minimum);
        g_assert_cmpint (request + minimum - 200, ==, slot);
        total += slot;
      }
      g_assert_cmpint (total, ==, width);
    }
  }
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/layout/measure-bounds", test_measure_bounds);
  g_test_add_func ("/layout/column-boundaries", test_column_boundaries);
  g_test_add_func ("/layout/chrome-accounting", test_chrome_accounting);
  return g_test_run ();
}

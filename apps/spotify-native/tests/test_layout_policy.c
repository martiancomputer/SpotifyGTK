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

static void test_artwork_read_ahead (void)
{
  /* A wheel landing beyond the viewport must not skip the very next rows. */
  SpotifyGtkArtworkWindow down = spotifygtk_layout_artwork_window (
    10000, 100, 114, 140, 8, 32, 16, FALSE);
  g_assert_cmpuint (down.first, <=, 100);
  g_assert_cmpuint (down.last, >=, 122);
  SpotifyGtkArtworkWindow up = spotifygtk_layout_artwork_window (
    10000, 140, 154, 100, 8, 32, 16, TRUE);
  g_assert_cmpuint (up.first, <=, 132);
  g_assert_cmpuint (up.last, >=, 154);

  /* Motion still changes the scan window with a fixed wheel destination. */
  SpotifyGtkArtworkWindow next = spotifygtk_layout_artwork_window (
    10000, 101, 115, 140, 8, 32, 16, FALSE);
  g_assert_cmpuint (next.first, !=, down.first);
  g_assert_cmpuint (next.last, !=, down.last);
}

static void test_artwork_window_bounds (void)
{
  const guint targets[] = { 0, 100, 5000, 9999 };
  for (guint first = 0; first < 10000; first += 13) {
    guint last = MIN (first + 14, 9999);
    for (guint t = 0; t < G_N_ELEMENTS (targets); t++) {
      for (guint up = 0; up < 2; up++) {
        SpotifyGtkArtworkWindow w = spotifygtk_layout_artwork_window (
          10000, first, last, targets[t], 8, 32, 16, up);
        g_assert_cmpuint (w.first, <=, first);
        g_assert_cmpuint (w.last, >=, last);
        g_assert_cmpuint (w.last, <, 10000);
        g_assert_cmpuint (w.last - w.first + 1, <=, last - first + 1 + 32 + 16);
      }
    }
  }
  SpotifyGtkArtworkWindow single = spotifygtk_layout_artwork_window (
    1, 0, 0, 0, 8, 32, 16, FALSE);
  g_assert_cmpuint (single.first, ==, 0);
  g_assert_cmpuint (single.last, ==, 0);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/layout/measure-bounds", test_measure_bounds);
  g_test_add_func ("/layout/column-boundaries", test_column_boundaries);
  g_test_add_func ("/layout/chrome-accounting", test_chrome_accounting);
  g_test_add_func ("/layout/artwork-read-ahead", test_artwork_read_ahead);
  g_test_add_func ("/layout/artwork-window-bounds", test_artwork_window_bounds);
  return g_test_run ();
}

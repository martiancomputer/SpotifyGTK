#include "ui/presentation_policy.h"

static void
test_ranges (void)
{
  const guint fps[] = { 0, 30, 60, 120, 144, 240, 360 };
  for (guint i = 0; i < G_N_ELEMENTS (fps); i++)
    g_assert_true (spotifygtk_presentation_valid_fps (fps[i]));
  g_assert_false (spotifygtk_presentation_valid_fps (29));
  g_assert_false (spotifygtk_presentation_valid_fps (31));
  g_assert_false (spotifygtk_presentation_valid_fps (361));
  const guint scale[] = { 0, 25, 50, 75, 90, 100, 150, 300 };
  for (guint i = 0; i < G_N_ELEMENTS (scale); i++)
    g_assert_true (spotifygtk_presentation_valid_scale (scale[i], FALSE));
  for (guint percent = 25; percent <= 300; percent++)
    g_assert_true (spotifygtk_presentation_valid_scale (percent, TRUE));
  g_assert_false (spotifygtk_presentation_valid_scale (0, TRUE));
  g_assert_false (spotifygtk_presentation_valid_scale (24, TRUE));
  g_assert_false (spotifygtk_presentation_valid_scale (301, TRUE));
  g_assert_false (spotifygtk_presentation_valid_scale (125, FALSE));
  g_assert_cmpuint (spotifygtk_presentation_step_scale (0, TRUE), ==, 110);
  g_assert_cmpuint (spotifygtk_presentation_step_scale (0, FALSE), ==, 90);
  g_assert_cmpuint (spotifygtk_presentation_step_scale (25, FALSE), ==, 25);
  g_assert_cmpuint (spotifygtk_presentation_step_scale (300, TRUE), ==, 300);
  g_assert_cmpuint (spotifygtk_presentation_step_scale (90, TRUE), ==, 100);
}

static void
test_geometry (void)
{
  g_assert_cmpfloat (spotifygtk_presentation_scale (0), ==, 1.0);
  g_assert_cmpfloat (spotifygtk_presentation_scale (300), ==, 3.0);
  g_assert_cmpfloat (spotifygtk_presentation_scale (301), ==, 1.0);
  g_assert_cmpint (spotifygtk_presentation_extent (-1, 3), ==, -1);
  g_assert_cmpint (spotifygtk_presentation_extent (0, 3), ==, 0);
  g_assert_cmpint (spotifygtk_presentation_extent (177, 0.25), ==, 45);
  g_assert_cmpint (spotifygtk_presentation_extent (177, 0.75), ==, 133);
  g_assert_cmpint (spotifygtk_presentation_extent (177, 1.5), ==, 266);
  g_assert_cmpint (spotifygtk_presentation_extent (177, 3), ==, 531);
  g_assert_cmpint (spotifygtk_presentation_extent (G_MAXINT, 3), ==, G_MAXINT);
  g_assert_cmpint (spotifygtk_presentation_extent (300, 1.37), ==, 411);
}

static void
test_logical_allocation (void)
{
  /* Default/100% is an exact identity allocation when content fits. */
  g_assert_cmpint (spotifygtk_presentation_logical_extent (1920, 1, 900), ==, 1920);
  g_assert_cmpint (spotifygtk_presentation_logical_extent (900, 1, 900), ==, 900);
  /* No tiny phantom scrollbar from rounding up fractional zoom. */
  const double scales[] = { 0.25, 0.5, 0.75, 1, 1.37, 1.5, 3 };
  for (guint i = 0; i < G_N_ELEMENTS (scales); i++) {
    for (int viewport = 30; viewport < 2000; viewport++) {
      int logical = spotifygtk_presentation_logical_extent (viewport, scales[i], 1);
      g_assert_cmpint (spotifygtk_presentation_extent (logical, scales[i]), <=, viewport);
    }
  }
  /* Real content minimums overflow predictably; scrollbar visibility cannot
   * reduce the viewport allocation and change these dimensions. */
  g_assert_cmpint (spotifygtk_presentation_logical_extent (900, 1.5, 900), ==, 900);
  g_assert_cmpint (spotifygtk_presentation_logical_extent (900, 3, 900), ==, 900);
  g_assert_cmpint (spotifygtk_presentation_logical_extent (0, 3, 0), ==, 1);
  g_assert_cmpint (spotifygtk_presentation_logical_extent (G_MAXINT, 0.25, 1), ==, G_MAXINT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/presentation/ranges", test_ranges);
  g_test_add_func ("/presentation/geometry", test_geometry);
  g_test_add_func ("/presentation/logical-allocation", test_logical_allocation);
  return g_test_run ();
}

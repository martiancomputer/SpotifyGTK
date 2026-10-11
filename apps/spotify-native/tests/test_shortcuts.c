#include "ui/shortcuts.h"

/* Parsing and matching need no display, widgets, synthesized events or GUI
 * initialization. Real scrolling remains a manual performance check. */
static void test_matching (void)
{
  g_assert_true (spotifygtk_shortcut_matches ("<Control>plus", GDK_KEY_plus, GDK_CONTROL_MASK | GDK_SHIFT_MASK));
  g_assert_true (spotifygtk_shortcut_matches ("<Control>plus", GDK_KEY_equal, GDK_CONTROL_MASK));
  g_assert_true (spotifygtk_shortcut_matches ("<Control>plus", GDK_KEY_KP_Add, GDK_CONTROL_MASK));
  g_assert_true (spotifygtk_shortcut_matches ("<Control>minus", GDK_KEY_KP_Subtract, GDK_CONTROL_MASK));
  g_assert_true (spotifygtk_shortcut_matches ("space", GDK_KEY_space, GDK_LOCK_MASK));
  g_assert_false (spotifygtk_shortcut_matches ("space", GDK_KEY_space, GDK_CONTROL_MASK));
  g_assert_false (spotifygtk_shortcut_matches ("<Control>plus", GDK_KEY_plus, GDK_CONTROL_MASK | GDK_ALT_MASK));
  g_assert_false (spotifygtk_shortcut_matches ("Escape", GDK_KEY_Escape, 0));
  g_assert_false (spotifygtk_shortcut_matches ("F11", GDK_KEY_F11, 0));
  g_assert_false (spotifygtk_shortcut_matches ("a", GDK_KEY_a, 0));
  g_assert_false (spotifygtk_shortcut_matches ("nonsense", GDK_KEY_space, 0));
  g_assert_false (spotifygtk_shortcut_is_typing (NULL));
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/shortcuts/matching", test_matching);
  return g_test_run ();
}

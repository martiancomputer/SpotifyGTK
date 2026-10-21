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

static void test_keycap_legends (void)
{
  g_auto(GStrv) plus = spotifygtk_shortcut_keycap_labels ("<Control>plus");
  g_assert_cmpuint (g_strv_length (plus), ==, 2);
  g_assert_cmpstr (plus[0], ==, "Ctrl");
  g_assert_cmpstr (plus[1], ==, "+");
  g_auto(GStrv) keypad = spotifygtk_shortcut_keycap_labels ("<Control>KP_Add");
  g_assert_cmpstr (keypad[1], ==, "+");
  g_auto(GStrv) space = spotifygtk_shortcut_keycap_labels ("space");
  g_assert_cmpuint (g_strv_length (space), ==, 1);
  g_assert_cmpstr (space[0], ==, "Space");
  g_auto(GStrv) custom = spotifygtk_shortcut_keycap_labels ("<Control><Alt>p");
  g_assert_cmpuint (g_strv_length (custom), ==, 3);
  g_assert_cmpstr (custom[0], ==, "Ctrl");
  g_assert_cmpstr (custom[1], ==, "Alt");
  g_assert_cmpstr (custom[2], ==, "P");
  g_auto(GStrv) invalid = spotifygtk_shortcut_keycap_labels ("Escape");
  g_assert_cmpuint (g_strv_length (invalid), ==, 0);
}

static void test_registry (void)
{
  g_autoptr(GHashTable) keys = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr(SpotifyGtkSettings) settings = g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  guint groups[SPOTIFYGTK_SHORTCUT_GROUP_COUNT] = { 0 };
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_COUNT; i++) {
    const SpotifyGtkShortcutInfo *info = spotifygtk_shortcut_info (i);
    g_assert_nonnull (info);
    g_assert_nonnull (info->key);
    g_assert_nonnull (info->label);
    g_assert_nonnull (info->description);
    g_assert_true (g_hash_table_add (keys, (gpointer) info->key));
    g_assert_cmpuint (info->group, <, SPOTIFYGTK_SHORTCUT_GROUP_COUNT);
    groups[info->group]++;
    guint key; GdkModifierType mods;
    g_assert_true (spotifygtk_shortcut_parse (info->accelerator, &key, &mods));
    g_assert_cmpuint (key, !=, GDK_KEY_Escape);
    g_assert_cmpuint (key, !=, GDK_KEY_F11);
    g_assert_cmpint (spotifygtk_shortcut_find_conflict (settings, i, info->accelerator), ==, -1);
    g_auto(GStrv) legends = spotifygtk_shortcut_keycap_labels (info->accelerator);
    g_assert_cmpuint (g_strv_length (legends), >, 0);
  }
  g_assert_cmpuint (SPOTIFYGTK_SHORTCUT_COUNT, ==, 23);
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_GROUP_COUNT; i++) g_assert_cmpuint (groups[i], >, 0);
  g_assert_null (spotifygtk_shortcut_info (SPOTIFYGTK_SHORTCUT_COUNT));
  g_assert_cmpint (spotifygtk_shortcut_find_conflict (settings, SPOTIFYGTK_SHORTCUT_SEARCH,
    "<Control>equal"), ==, SPOTIFYGTK_SHORTCUT_SCALE_UP);
  g_assert_cmpint (spotifygtk_shortcut_find_conflict (settings, SPOTIFYGTK_SHORTCUT_SEARCH,
    "<Control><Shift>equal"), ==, SPOTIFYGTK_SHORTCUT_SCALE_UP);
  g_assert_cmpint (spotifygtk_shortcut_find_conflict (settings, SPOTIFYGTK_SHORTCUT_SEARCH,
    "<Control>KP_Add"), ==, SPOTIFYGTK_SHORTCUT_SCALE_UP);
}

static void test_typing_precedence (void)
{
  g_assert_false (spotifygtk_shortcut_allowed_while_typing (SPOTIFYGTK_SHORTCUT_PLAY_PAUSE, GDK_KEY_space, 0));
  g_assert_false (spotifygtk_shortcut_allowed_while_typing (SPOTIFYGTK_SHORTCUT_NEXT, GDK_KEY_Right, GDK_CONTROL_MASK));
  g_assert_false (spotifygtk_shortcut_allowed_while_typing (SPOTIFYGTK_SHORTCUT_SEEK_FORWARD, GDK_KEY_Right, GDK_SHIFT_MASK));
  g_assert_false (spotifygtk_shortcut_allowed_while_typing (SPOTIFYGTK_SHORTCUT_HOME, GDK_KEY_a, GDK_CONTROL_MASK));
  g_assert_false (spotifygtk_shortcut_allowed_while_typing (SPOTIFYGTK_SHORTCUT_SEARCH, GDK_KEY_space, 0));
  g_assert_true (spotifygtk_shortcut_allowed_while_typing (SPOTIFYGTK_SHORTCUT_SEARCH, GDK_KEY_2, GDK_CONTROL_MASK));
  g_assert_true (spotifygtk_shortcut_allowed_while_typing (SPOTIFYGTK_SHORTCUT_FOCUS_SEARCH, GDK_KEY_f, GDK_CONTROL_MASK));
  g_assert_true (spotifygtk_shortcut_allowed_while_typing (SPOTIFYGTK_SHORTCUT_SCALE_UP, GDK_KEY_plus, GDK_CONTROL_MASK | GDK_SHIFT_MASK));
  g_assert_true (spotifygtk_shortcut_matches ("<Shift>Right", GDK_KEY_Right, GDK_SHIFT_MASK));
  g_assert_false (spotifygtk_shortcut_matches ("<Shift>Right", GDK_KEY_Right, 0));
}

static void test_restore_defaults (void)
{
  g_autoptr(SpotifyGtkSettings) settings = g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  spotifygtk_settings_set_ui_scale (settings, 90, FALSE);
  spotifygtk_settings_set_volume (settings, 37);
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_COUNT; i++) {
    g_autofree gchar *custom = g_strdup_printf ("<Control><Alt>F%u", i + 1);
    spotifygtk_settings_set_shortcut (settings, i, custom);
    g_assert_cmpstr (spotifygtk_settings_get_shortcut (settings, i), ==, custom);
  }
  spotifygtk_shortcuts_restore_defaults (settings);
  spotifygtk_settings_flush (settings);
  g_autoptr(SpotifyGtkSettings) reloaded = g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_COUNT; i++) {
    const gchar *expected = spotifygtk_shortcut_info (i)->accelerator;
    g_assert_cmpstr (spotifygtk_settings_get_shortcut (settings, i), ==, expected);
    g_assert_cmpstr (spotifygtk_settings_get_shortcut (reloaded, i), ==, expected);
  }
  g_assert_cmpuint (spotifygtk_settings_get_ui_scale (reloaded), ==, 90);
  g_assert_cmpuint (spotifygtk_settings_get_volume (reloaded), ==, 37);
}

static void test_search (void)
{
  g_assert_true (spotifygtk_shortcut_search_matches (
    NULL, "Play / Pause", "Toggle playback", "space"));
  g_assert_true (spotifygtk_shortcut_search_matches (
    " \t\n", "Play / Pause", "Toggle playback", "space"));
  g_assert_true (spotifygtk_shortcut_search_matches (
    "PAUSE play", "Play / Pause", "Toggle playback", "space"));
  g_assert_true (spotifygtk_shortcut_search_matches (
    "toggle", "Play / Pause", "Toggle playback", "space"));
  g_assert_true (spotifygtk_shortcut_search_matches (
    "space", "Play / Pause", "Toggle playback", "space"));
  g_assert_true (spotifygtk_shortcut_search_matches (
    "Ctrl", "Next track", "", "<Control>Right"));
  g_assert_true (spotifygtk_shortcut_search_matches (
    "control right", "Next track", "", "<Control>Right"));
  g_assert_false (spotifygtk_shortcut_search_matches (
    "control left", "Next track", "", "<Control>Right"));
  g_assert_true (spotifygtk_shortcut_search_matches (
    "F7", "Next track", "", "<Alt>F7"));
  g_assert_false (spotifygtk_shortcut_search_matches (
    "control", "Next track", "", "<Alt>F7"));
  g_assert_true (spotifygtk_shortcut_search_matches (
    "écoute", "ÉCOUTE", NULL, NULL));
  g_assert_true (spotifygtk_shortcut_search_matches (
    "esc", "Cancel", "Reserved, not rebindable", "Escape"));
  g_assert_false (spotifygtk_shortcut_search_matches (
    "no such shortcut", "Play / Pause", "Toggle playback", "space"));
}

int main (int argc, char **argv)
{
  g_autofree gchar *temporary = g_dir_make_tmp ("spotifygtk-shortcut-settings-XXXXXX", NULL);
  g_assert_nonnull (temporary);
  g_setenv ("XDG_CONFIG_HOME", temporary, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/shortcuts/matching", test_matching);
  g_test_add_func ("/shortcuts/keycap-legends", test_keycap_legends);
  g_test_add_func ("/shortcuts/registry", test_registry);
  g_test_add_func ("/shortcuts/typing-precedence", test_typing_precedence);
  g_test_add_func ("/shortcuts/restore-defaults", test_restore_defaults);
  g_test_add_func ("/shortcuts/search", test_search);
  return g_test_run ();
}

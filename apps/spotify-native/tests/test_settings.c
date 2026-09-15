#include <glib.h>
#include <glib/gstdio.h>

#include "ui/settings.h"

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
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (first), ==, 19);
  spotifygtk_settings_set_lyrics_font_size (first, 28);
  spotifygtk_settings_set_lyrics_font_size (first, 29);
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (first), ==, 28);
  spotifygtk_settings_set_online_lyrics (first, TRUE);
  g_assert_true (spotifygtk_settings_get_online_lyrics (first));

  g_autoptr(SpotifyGtkSettings) second =
    g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  g_assert_true (spotifygtk_settings_get_online_lyrics (second));
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (second), ==, 28);
  spotifygtk_settings_set_lyrics_font_size (second, 19);
  spotifygtk_settings_set_online_lyrics (second, FALSE);
  g_autoptr(SpotifyGtkSettings) third =
    g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  g_assert_false (spotifygtk_settings_get_online_lyrics (third));
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

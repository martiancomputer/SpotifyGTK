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
  g_assert_true (spotifygtk_settings_get_compact_mode (first));
  g_assert_true (spotifygtk_settings_get_show_playlists_separately (first));
  g_assert_true (spotifygtk_settings_get_page_crossfade (first));
  g_assert_false (spotifygtk_settings_get_online_lyrics (first));
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (first), ==, 19);
  g_assert_cmpint (spotifygtk_settings_get_sample_format (first), ==,
                   SPOTIFYGTK_SAMPLE_FORMAT_16);
  g_assert_cmpint (spotifygtk_settings_get_resampler_mode (first), ==,
                   SPOTIFYGTK_RESAMPLER_POLYPHASE);
  g_assert_cmpuint (spotifygtk_settings_get_frame_rate (first), ==, 0);
  g_assert_false (spotifygtk_settings_get_unsynchronized_rendering (first));
  spotifygtk_settings_set_unsynchronized_rendering (first, TRUE);
  g_assert_cmpuint (spotifygtk_settings_get_ui_scale (first), ==, 0);
  g_assert_false (spotifygtk_settings_get_ui_scale_custom (first));
  g_assert_cmpuint (spotifygtk_settings_get_volume (first), ==, 100);
  spotifygtk_settings_set_volume (first, 37);
  spotifygtk_settings_set_volume (first, 101);
  spotifygtk_settings_flush (first);
  spotifygtk_settings_set_shortcut (first, SPOTIFYGTK_SHORTCUT_SCALE_UP, "<Control>F2");
  spotifygtk_settings_set_shortcut (first, SPOTIFYGTK_SHORTCUT_SEARCH, "<Control>F3");
  g_assert_cmpstr (spotifygtk_settings_get_shortcut (first, SPOTIFYGTK_SHORTCUT_SEEK_FORWARD), ==, "<Shift>Right");
  g_assert_cmpstr (spotifygtk_settings_get_shortcut (first, SPOTIFYGTK_SHORTCUT_PLAY_PAUSE), ==, "space");
  spotifygtk_settings_set_ui_scale (first, 90, FALSE);
  g_assert_cmpuint (spotifygtk_settings_get_ui_scale (first), ==, 90);
  const guint rates[] = { 30, 60, 120, 144, 240, 360 };
  for (guint i = 0; i < G_N_ELEMENTS (rates); i++) {
    spotifygtk_settings_set_frame_rate (first, rates[i]);
    g_assert_cmpuint (spotifygtk_settings_get_frame_rate (first), ==, rates[i]);
  }
  spotifygtk_settings_set_frame_rate (first, 29);
  spotifygtk_settings_set_frame_rate (first, 361);
  spotifygtk_settings_set_frame_rate (first, G_MAXUINT);
  g_assert_cmpuint (spotifygtk_settings_get_frame_rate (first), ==, 360);
  spotifygtk_settings_set_ui_scale (first, 137, TRUE);
  spotifygtk_settings_set_ui_scale (first, 24, TRUE);
  spotifygtk_settings_set_ui_scale (first, 301, TRUE);
  spotifygtk_settings_set_ui_scale (first, 137, FALSE);
  g_assert_cmpuint (spotifygtk_settings_get_ui_scale (first), ==, 137);
  g_assert_true (spotifygtk_settings_get_ui_scale_custom (first));
  spotifygtk_settings_set_lyrics_font_size (first, 28);
  spotifygtk_settings_set_lyrics_font_size (first, 29);
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (first), ==, 28);
  spotifygtk_settings_set_online_lyrics (first, TRUE);
  spotifygtk_settings_set_compact_mode (first, FALSE);
  spotifygtk_settings_set_show_playlists_separately (first, FALSE);
  spotifygtk_settings_set_page_crossfade (first, FALSE);
  spotifygtk_settings_set_sample_rate (first, SPOTIFYGTK_SAMPLE_RATE_384000);
  spotifygtk_settings_set_sample_format (first, SPOTIFYGTK_SAMPLE_FORMAT_32);
  spotifygtk_settings_set_resampler_mode (first, SPOTIFYGTK_RESAMPLER_LINEAR);
  g_assert_false (spotifygtk_settings_get_compact_mode (first));
  g_assert_false (spotifygtk_settings_get_show_playlists_separately (first));
  g_assert_true (spotifygtk_settings_get_online_lyrics (first));

  g_autoptr(SpotifyGtkSettings) second =
    g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  g_assert_cmpuint (spotifygtk_settings_get_volume (second), ==, 37);
  g_assert_cmpstr (spotifygtk_settings_get_shortcut (second, SPOTIFYGTK_SHORTCUT_SCALE_UP), ==, "<Control>F2");
  g_assert_cmpstr (spotifygtk_settings_get_shortcut (second, SPOTIFYGTK_SHORTCUT_SEARCH), ==, "<Control>F3");
  spotifygtk_settings_set_shortcut (second, SPOTIFYGTK_SHORTCUT_SEARCH, NULL);
  g_assert_cmpstr (spotifygtk_settings_get_shortcut (second, SPOTIFYGTK_SHORTCUT_SEARCH), ==, "<Control>2");
  spotifygtk_settings_set_shortcut (second, SPOTIFYGTK_SHORTCUT_SCALE_UP, NULL);
  spotifygtk_settings_set_volume (second, 0);
  spotifygtk_settings_flush (second);
  g_assert_cmpuint (spotifygtk_settings_get_frame_rate (second), ==, 360);
  g_assert_true (spotifygtk_settings_get_unsynchronized_rendering (second));
  spotifygtk_settings_set_unsynchronized_rendering (second, FALSE);
  spotifygtk_settings_set_frame_rate (second, 0);
  g_assert_cmpuint (spotifygtk_settings_get_ui_scale (second), ==, 137);
  g_assert_true (spotifygtk_settings_get_ui_scale_custom (second));
  spotifygtk_settings_set_ui_scale (second, 0, FALSE);
  g_assert_true (spotifygtk_settings_get_online_lyrics (second));
  g_assert_false (spotifygtk_settings_get_compact_mode (second));
  g_assert_false (spotifygtk_settings_get_show_playlists_separately (second));
  g_assert_false (spotifygtk_settings_get_page_crossfade (second));
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (second), ==, 28);
  g_assert_cmpint (spotifygtk_settings_get_sample_rate (second), ==,
                   SPOTIFYGTK_SAMPLE_RATE_384000);
  g_assert_cmpint (spotifygtk_settings_sample_rate_hz (
                    spotifygtk_settings_get_sample_rate (second)), ==, 384000);
  g_assert_cmpint (spotifygtk_settings_get_sample_format (second), ==,
                   SPOTIFYGTK_SAMPLE_FORMAT_32);
  g_assert_cmpint (spotifygtk_settings_get_resampler_mode (second), ==,
                   SPOTIFYGTK_RESAMPLER_LINEAR);
  spotifygtk_settings_set_lyrics_font_size (second, 19);
  spotifygtk_settings_set_online_lyrics (second, FALSE);
  spotifygtk_settings_set_compact_mode (second, TRUE);
  spotifygtk_settings_set_show_playlists_separately (second, TRUE);
  spotifygtk_settings_set_page_crossfade (second, TRUE);
  spotifygtk_settings_set_sample_rate (second, SPOTIFYGTK_SAMPLE_RATE_DEFAULT);
  spotifygtk_settings_set_sample_format (second, SPOTIFYGTK_SAMPLE_FORMAT_16);
  spotifygtk_settings_set_resampler_mode (second, SPOTIFYGTK_RESAMPLER_POLYPHASE);
  g_autoptr(SpotifyGtkSettings) third =
    g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  g_assert_cmpuint (spotifygtk_settings_get_volume (third), ==, 0);
  g_assert_cmpstr (spotifygtk_settings_get_shortcut (third, SPOTIFYGTK_SHORTCUT_SCALE_UP), ==, "<Control>plus");
  g_assert_cmpuint (spotifygtk_settings_get_frame_rate (third), ==, 0);
  g_assert_false (spotifygtk_settings_get_unsynchronized_rendering (third));
  g_assert_cmpuint (spotifygtk_settings_get_ui_scale (third), ==, 0);
  g_assert_false (spotifygtk_settings_get_ui_scale_custom (third));
  spotifygtk_settings_set_ui_scale (third, 100, TRUE);
  g_autoptr(SpotifyGtkSettings) custom = g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
  g_assert_cmpuint (spotifygtk_settings_get_ui_scale (custom), ==, 100);
  g_assert_true (spotifygtk_settings_get_ui_scale_custom (custom));
  g_assert_false (spotifygtk_settings_get_online_lyrics (third));
  g_assert_true (spotifygtk_settings_get_compact_mode (third));
  g_assert_true (spotifygtk_settings_get_show_playlists_separately (third));
  g_assert_true (spotifygtk_settings_get_page_crossfade (third));
  g_assert_cmpuint (spotifygtk_settings_get_lyrics_font_size (third), ==, 19);
  g_assert_cmpint (spotifygtk_settings_get_sample_rate (third), ==,
                   SPOTIFYGTK_SAMPLE_RATE_DEFAULT);
  g_assert_cmpint (spotifygtk_settings_get_sample_format (third), ==,
                   SPOTIFYGTK_SAMPLE_FORMAT_16);
  g_assert_cmpint (spotifygtk_settings_get_resampler_mode (third), ==,
                   SPOTIFYGTK_RESAMPLER_POLYPHASE);

  g_autofree gchar *config = g_build_filename (
    temporary, "spotify-native", "settings.ini", NULL);
  g_autofree gchar *config_dir = g_build_filename (
    temporary, "spotify-native", NULL);
  g_autoptr(GKeyFile) corrupt = g_key_file_new ();
  const gint invalid[] = { -1, 1, 24, 301, G_MAXINT };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++) {
    g_key_file_set_integer (corrupt, "spotify-native", "frame-rate", invalid[i]);
    g_key_file_set_integer (corrupt, "spotify-native", "ui-scale", invalid[i]);
    g_key_file_set_boolean (corrupt, "spotify-native", "ui-scale-custom", TRUE);
    g_assert_true (g_key_file_save_to_file (corrupt, config, &error));
    g_assert_no_error (error);
    g_autoptr(SpotifyGtkSettings) invalid_settings = g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);
    g_assert_cmpuint (spotifygtk_settings_get_frame_rate (invalid_settings), ==, 0);
    g_assert_cmpuint (spotifygtk_settings_get_ui_scale (invalid_settings), ==, 0);
    g_assert_false (spotifygtk_settings_get_ui_scale_custom (invalid_settings));
  }
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

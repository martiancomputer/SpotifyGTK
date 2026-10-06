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

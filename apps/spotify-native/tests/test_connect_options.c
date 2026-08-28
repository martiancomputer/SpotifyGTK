#include <glib.h>

#include "spotify/connect_options.h"
#include "spotify/protobuf_min.h"

static GByteArray *
mode_entry (const gchar *value)
{
  GByteArray *entry = g_byte_array_new ();
  pb_write_bytes_field (entry, 1, (const guint8 *) "context_enhancement",
                        strlen ("context_enhancement"));
  pb_write_bytes_field (entry, 2, (const guint8 *) value, strlen (value));
  return entry;
}

static void
test_player_options (void)
{
  g_autoptr(GByteArray) options = g_byte_array_new ();
  g_autoptr(GByteArray) mode = mode_entry ("RECOMMENDATION");
  pb_write_varint_field (options, 1, 1);
  pb_write_message_field (options, 5, mode->data, mode->len);
  g_assert_cmpuint (spotifygtk_connect_player_shuffle_mode (
                      options->data, options->len, 0), ==, 2);

  g_byte_array_set_size (options, 0);
  g_clear_pointer (&mode, g_byte_array_unref);
  mode = mode_entry ("NONE");
  pb_write_varint_field (options, 1, 1);
  pb_write_message_field (options, 5, mode->data, mode->len);
  g_assert_cmpuint (spotifygtk_connect_player_shuffle_mode (
                      options->data, options->len, 2), ==, 1);

  g_byte_array_set_size (options, 0);
  pb_write_varint_field (options, 1, 0);
  g_assert_cmpuint (spotifygtk_connect_player_shuffle_mode (
                      options->data, options->len, 2), ==, 0);
}

static void
test_set_options_request (void)
{
  g_autoptr(GByteArray) options = g_byte_array_new ();
  g_autoptr(GByteArray) wrapped = g_byte_array_new ();
  g_autoptr(GByteArray) mode = mode_entry ("RECOMMENDATION");
  pb_write_varint_field (wrapped, 1, 1);
  pb_write_message_field (options, 3, wrapped->data, wrapped->len);
  pb_write_message_field (options, 7, mode->data, mode->len);
  g_assert_cmpuint (spotifygtk_connect_command_shuffle_mode (
                      options->data, options->len, 0), ==, 2);

  g_byte_array_set_size (options, 0);
  g_byte_array_set_size (wrapped, 0);
  g_clear_pointer (&mode, g_byte_array_unref);
  mode = mode_entry ("NONE");
  pb_write_varint_field (wrapped, 1, 1);
  pb_write_message_field (options, 3, wrapped->data, wrapped->len);
  pb_write_message_field (options, 7, mode->data, mode->len);
  g_assert_cmpuint (spotifygtk_connect_command_shuffle_mode (
                      options->data, options->len, 2), ==, 1);

  g_byte_array_set_size (options, 0);
  g_byte_array_set_size (wrapped, 0);
  pb_write_varint_field (wrapped, 1, 0);
  pb_write_message_field (options, 3, wrapped->data, wrapped->len);
  g_assert_cmpuint (spotifygtk_connect_command_shuffle_mode (
                      options->data, options->len, 2), ==, 0);
}

static void
test_legacy_command (void)
{
  g_autoptr(GByteArray) options = g_byte_array_new ();
  pb_write_varint_field (options, 1, 1);
  g_assert_cmpuint (spotifygtk_connect_command_shuffle_mode (
                      options->data, options->len, 0), ==, 1);
}

static void
test_live_position (void)
{
  /* A keepalive 30 seconds after a track-start report must publish 0:30, not
   * pair the original 0:00 with a fresh PlayerState timestamp. */
  g_assert_cmpint (spotifygtk_connect_live_position (0, 1000, 31000, TRUE),
                   ==, 30000);
  g_assert_cmpint (spotifygtk_connect_live_position (42000, 1000, 31000, TRUE),
                   ==, 72000);

  /* Paused clocks stay fixed, and a clock anomaly never moves backwards. */
  g_assert_cmpint (spotifygtk_connect_live_position (42000, 1000, 31000, FALSE),
                   ==, 42000);
  g_assert_cmpint (spotifygtk_connect_live_position (42000, 31000, 1000, TRUE),
                   ==, 42000);

  g_assert_cmpint (spotifygtk_connect_live_position (
                     G_MAXINT64 - 10, 1000, 31000, TRUE), ==, G_MAXINT64);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/connect-options/player-state", test_player_options);
  g_test_add_func ("/connect-options/set-options-request",
                   test_set_options_request);
  g_test_add_func ("/connect-options/legacy-command", test_legacy_command);
  g_test_add_func ("/connect-options/live-position", test_live_position);
  return g_test_run ();
}

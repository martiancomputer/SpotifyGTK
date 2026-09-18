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

static void
test_transfer_position (void)
{
  guint64 latest = 1000;
  g_assert_false (spotifygtk_connect_accept_timestamp (&latest, 999));
  g_assert_true (spotifygtk_connect_accept_timestamp (&latest, 1000));
  g_assert_true (spotifygtk_connect_accept_timestamp (&latest, 1001));
  g_assert_true (spotifygtk_connect_accept_timestamp (&latest, 0));
  g_assert_cmpuint (latest, ==, 1001);
  g_assert_cmpint (spotifygtk_connect_transfer_position (92000, 0, TRUE), ==, 92000);
  g_assert_cmpint (spotifygtk_connect_transfer_position (92000, -1, TRUE), ==, 92000);
  g_assert_cmpint (spotifygtk_connect_transfer_position (92000, 45000, TRUE), ==, 92000);
  g_assert_cmpint (spotifygtk_connect_transfer_position (92000, 45000, FALSE), ==, 45000);
  g_assert_cmpint (spotifygtk_connect_transfer_position (92000, -1, FALSE), ==, 0);
}

static void
test_queue_wire (void)
{
  const gchar *uri = "spotify:track:1234567890123456789012";
  g_autoptr(GByteArray) state = g_byte_array_new ();
  spotifygtk_connect_write_track (state, 20, uri, "queue", 0);
  const guint8 *track, *value;
  gsize len, size;
  g_assert_true (pb_find_bytes_field (state->data, state->len, 20, &track, &len));
  g_assert_true (pb_find_bytes_field (track, len, 1, &value, &size));
  g_assert_cmpmem (value, size, uri, strlen (uri));
  g_assert_true (pb_find_bytes_field (track, len, 6, &value, &size));
  g_assert_cmpmem (value, size, "queue", 5);
  g_autofree gchar *a = spotifygtk_connect_track_uid (uri, "queue", 0);
  g_autofree gchar *b = spotifygtk_connect_track_uid (uri, "queue", 1);
  g_autofree gchar *c = spotifygtk_connect_track_uid (uri, "context", 0);
  g_autofree gchar *current = spotifygtk_connect_track_uid (uri, "context", 20000);
  g_autofree gchar *previous = spotifygtk_connect_track_uid (uri, "context", 10000);
  g_assert_cmpstr (a, !=, b); g_assert_cmpstr (a, !=, c);
  g_assert_cmpstr (current, !=, c); g_assert_cmpstr (previous, !=, c);
  g_assert_cmpstr (current, !=, previous);
  g_assert_true (pb_find_bytes_field (track, len, 2, &value, &size));
  g_assert_cmpmem (value, size, a, strlen (a));
  const gchar *next[] = { uri, uri, NULL };
  g_autofree gchar *revision = spotifygtk_connect_queue_revision (uri, next, 2);
  g_autofree gchar *same = spotifygtk_connect_queue_revision (uri, next, 2);
  g_autofree gchar *different_provider = spotifygtk_connect_queue_revision (uri, next, 1);
  g_assert_cmpstr (revision, ==, same);
  g_assert_cmpstr (revision, !=, different_provider);
}

static void
test_json_queue (void)
{
  g_autoptr(GError) error = NULL;
  SpotifyConnectCommand *cmd = spotifygtk_connect_parse_command (
    "{\"endpoint\":\"set_queue\",\"queue_revision\":\"abc\",\"prev_tracks\":[],\"next_tracks\":["
    "{\"uri\":\"spotify:track:1234567890123456789012\",\"uid\":\"one\",\"provider\":\"queue\"},"
    "{\"uri\":\"spotify:track:1234567890123456789012\",\"uid\":\"two\",\"provider\":\"queue\"}]}", &error);
  g_assert_no_error (error); g_assert_nonnull (cmd);
  g_assert_cmpuint (cmd->next->len, ==, 2);
  g_assert_cmpstr (((SpotifyConnectTrack *) g_ptr_array_index (cmd->next, 1))->uid, ==, "two");
  g_assert_cmpstr (cmd->queue_revision, ==, "abc");
  spotifygtk_connect_command_free (cmd);
  cmd = spotifygtk_connect_parse_command ("{\"endpoint\":\"set_queue\",\"next_tracks\":[]}", &error);
  g_assert_no_error (error); g_assert_nonnull (cmd); g_assert_cmpuint (cmd->next->len, ==, 0);
  spotifygtk_connect_command_free (cmd);
}

static void
test_json_play_and_options (void)
{
  g_autoptr(GError) error = NULL;
  SpotifyConnectCommand *cmd = spotifygtk_connect_parse_command (
    "{\"endpoint\":\"play\",\"context\":{\"uri\":\"spotify:album:1234567890123456789012\"},"
    "\"options\":{\"skip_to\":{\"track_index\":3},\"seek_to\":92000,\"initially_paused\":true,"
    "\"player_options_override\":{\"shuffling_context\":false}}}", &error);
  g_assert_no_error (error); g_assert_nonnull (cmd);
  g_assert_cmpint (cmd->track_index, ==, 3); g_assert_cmpint (cmd->seek_ms, ==, 92000);
  g_assert_true (cmd->paused); g_assert_cmpint (cmd->shuffle_mode, ==, 0);
  spotifygtk_connect_command_free (cmd);
  cmd = spotifygtk_connect_parse_command ("{\"endpoint\":\"set_options\",\"repeating_track\":true}", &error);
  g_assert_no_error (error); g_assert_nonnull (cmd);
  g_assert_cmpint (cmd->shuffle_mode, ==, -1); /* A partial update must not disable shuffle. */
  g_assert_cmpint (cmd->repeating_track, ==, 1);
  spotifygtk_connect_command_free (cmd);
  cmd = spotifygtk_connect_parse_command ("{\"endpoint\":\"seek_to\",\"value\":0,\"options\":{}}", &error);
  g_assert_no_error (error); g_assert_nonnull (cmd); g_assert_cmpint (cmd->seek_ms, ==, 0);
  spotifygtk_connect_command_free (cmd);
  cmd = spotifygtk_connect_parse_command ("{\"endpoint\":\"skip_next\",\"options\":{\"skip_to\":{\"track_uid\":\"duplicate-2\"}}}", &error);
  g_assert_no_error (error); g_assert_nonnull (cmd);
  g_assert_cmpstr (cmd->track_uid, ==, "duplicate-2"); g_assert_null (cmd->uri);
  spotifygtk_connect_command_free (cmd);
}

static void
test_json_rejects_malformed (void)
{
  const gchar *bad[] = { "[]", "{", "{\"endpoint\":5}",
    "{\"endpoint\":\"set_queue\"}", "{\"endpoint\":\"add_to_queue\",\"track\":null}",
    "{\"endpoint\":\"set_queue\",\"next_tracks\":{}}",
    "{\"endpoint\":\"set_queue\",\"next_tracks\":[null]}",
    "{\"endpoint\":\"add_to_queue\",\"track\":{\"uri\":\"https://example.com\"}}" };
  for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
    g_autoptr(GError) error = NULL;
    SpotifyConnectCommand *cmd = spotifygtk_connect_parse_command (bad[i], &error);
    g_assert_null (cmd); g_assert_nonnull (error);
  }
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
  g_test_add_func ("/connect/transfer-position", test_transfer_position);
  g_test_add_func ("/connect/queue-wire", test_queue_wire);
  g_test_add_func ("/connect/json-queue", test_json_queue);
  g_test_add_func ("/connect/json-play-options", test_json_play_and_options);
  g_test_add_func ("/connect/json-malformed", test_json_rejects_malformed);
  return g_test_run ();
}

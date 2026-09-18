#include "connect_options.h"

#include "protobuf_min.h"

#include <string.h>

#define CTXOPT_SHUFFLING       1
#define CTXOPT_MODES           5
#define SETOPT_SHUFFLING       3
#define SETOPT_MODES           7
#define OPTIONAL_BOOL_VALUE    1
#define MAP_KEY                1
#define MAP_VALUE              2

/* Read context_enhancement from one protobuf map field. The same map occurs
 * at field 5 in ContextPlayerOptions and field 7 in SetOptionsRequest. */
static gboolean
options_enhancement (const guint8 *options, gsize len, guint modes_field,
                     gboolean *recommendation)
{
  gsize pos = 0;
  guint32 fn;
  PbWireType wt;
  const guint8 *field;
  gsize field_len;
  guint64 value;

  while (pb_read_field (options, len, &pos, &fn, &wt,
                        &field, &field_len, &value)) {
    if (fn != modes_field || wt != PB_WIRE_LENGTH_DELIMITED)
      continue;

    const guint8 *key = NULL, *val = NULL;
    gsize key_len = 0, val_len = 0;
    if (pb_find_bytes_field (field, field_len, MAP_KEY, &key, &key_len) &&
        pb_find_bytes_field (field, field_len, MAP_VALUE, &val, &val_len) &&
        key_len == strlen ("context_enhancement") &&
        memcmp (key, "context_enhancement", key_len) == 0) {
      *recommendation =
        val_len == strlen ("RECOMMENDATION") &&
        memcmp (val, "RECOMMENDATION", val_len) == 0;
      return TRUE;
    }
  }

  return FALSE;
}

guint
spotifygtk_connect_player_shuffle_mode (const guint8 *options, gsize len,
                                        guint fallback)
{
  guint64 shuffling = 0;
  gboolean has_shuffle = pb_find_varint_field (options, len,
                                                CTXOPT_SHUFFLING, &shuffling);
  gboolean recommendation = FALSE;
  gboolean has_enhancement = options_enhancement (options, len, CTXOPT_MODES,
                                                   &recommendation);

  if (recommendation)
    return 2;
  if (has_shuffle)
    return shuffling ? 1 : 0;
  if (has_enhancement && fallback == 2)
    return 1;
  return fallback;
}

guint
spotifygtk_connect_command_shuffle_mode (const guint8 *options, gsize len,
                                         guint fallback)
{
  /* A bare field 1 is retained for set_shuffling_context from older
   * controllers. In SetOptionsRequest field 1 is a message, so it cannot be
   * mistaken for this varint. */
  guint64 shuffling = 0;
  gboolean has_shuffle = pb_find_varint_field (options, len,
                                                CTXOPT_SHUFFLING, &shuffling);
  const guint8 *wrapped = NULL;
  gsize wrapped_len = 0;
  if (pb_find_bytes_field (options, len, SETOPT_SHUFFLING,
                           &wrapped, &wrapped_len))
    has_shuffle = pb_find_varint_field (wrapped, wrapped_len,
                                        OPTIONAL_BOOL_VALUE, &shuffling);

  gboolean recommendation = FALSE;
  gboolean has_enhancement = options_enhancement (options, len, SETOPT_MODES,
                                                   &recommendation);
  if (recommendation)
    return 2;
  if (has_shuffle)
    return shuffling ? 1 : 0;
  if (has_enhancement && fallback == 2)
    return 1;
  return fallback;
}

gint64
spotifygtk_connect_live_position (gint64 position_ms, gint64 observed_at_ms,
                                  gint64 now_ms, gboolean playing)
{
  position_ms = MAX (position_ms, 0);
  if (!playing || observed_at_ms <= 0 || now_ms <= observed_at_ms)
    return position_ms;

  gint64 elapsed_ms = now_ms - observed_at_ms;
  if (elapsed_ms > G_MAXINT64 - position_ms)
    return G_MAXINT64;
  return position_ms + elapsed_ms;
}

gint64
spotifygtk_connect_transfer_position (gint64 local_ms, gint64 remote_ms, gboolean same_track)
{
  if (same_track) return MAX (0, local_ms);
  return MAX (0, remote_ms);
}

gchar *
spotifygtk_connect_track_uid (const gchar *uri, const gchar *provider, guint index)
{
  g_autofree gchar *identity = g_strdup_printf ("%s:%u:%s", provider, index, uri);
  return g_compute_checksum_for_string (G_CHECKSUM_SHA1, identity, -1);
}

gboolean
spotifygtk_connect_accept_timestamp (guint64 *latest, guint64 received)
{
  if (received && received < *latest) return FALSE;
  if (received) *latest = received;
  return TRUE;
}

void
spotifygtk_connect_write_track (GByteArray *state, guint field, const gchar *uri,
                                const gchar *provider, guint index)
{
  if (!uri || !*uri) return;
  g_autoptr(GByteArray) track = g_byte_array_new ();
  g_autofree gchar *uid = spotifygtk_connect_track_uid (uri, provider, index);
  pb_write_bytes_field (track, 1, (const guint8 *) uri, strlen (uri));
  pb_write_bytes_field (track, 2, (const guint8 *) uid, strlen (uid));
  pb_write_bytes_field (track, 6, (const guint8 *) provider, strlen (provider));
  pb_write_message_field (state, field, track->data, track->len);
}

gchar *
spotifygtk_connect_queue_revision (const gchar *current_uri, const gchar *const *next, guint queued)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (current_uri) g_checksum_update (checksum, (const guchar *) current_uri, strlen (current_uri) + 1);
  for (guint i = 0; next && next[i]; i++)
    g_checksum_update (checksum, (const guchar *) next[i], strlen (next[i]) + 1);
  g_autofree gchar *providers = g_strdup_printf ("%u", queued);
  g_checksum_update (checksum, (const guchar *) providers, strlen (providers));
  return g_strdup (g_checksum_get_string (checksum));
}

static JsonObject *
command_object (JsonObject *object, const gchar *key)
{
  JsonNode *node = object ? json_object_get_member (object, key) : NULL;
  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

static const gchar *
command_string (JsonObject *object, const gchar *key)
{
  JsonNode *node = object ? json_object_get_member (object, key) : NULL;
  return node && json_node_get_value_type (node) == G_TYPE_STRING ? json_node_get_string (node) : NULL;
}

static gint64
command_number (JsonObject *object, const gchar *key, gint64 fallback)
{
  JsonNode *node = object ? json_object_get_member (object, key) : NULL;
  return node && json_node_get_value_type (node) == G_TYPE_INT64 ? json_node_get_int (node) : fallback;
}

static gint
command_bool (JsonObject *object, const gchar *key)
{
  JsonNode *node = object ? json_object_get_member (object, key) : NULL;
  if (node && JSON_NODE_HOLDS_OBJECT (node))
    node = json_object_get_member (json_node_get_object (node), "value");
  return node && json_node_get_value_type (node) == G_TYPE_BOOLEAN ? json_node_get_boolean (node) : -1;
}

static void
connect_track_free (gpointer data)
{
  SpotifyConnectTrack *track = data;
  g_free (track->uri); g_free (track->uid); g_free (track->provider); g_free (track);
}

static gboolean
command_track (GPtrArray *tracks, JsonObject *object)
{
  const gchar *uri = command_string (object, "uri");
  if (!uri || !g_str_has_prefix (uri, "spotify:track:") || strlen (uri) != 36) return FALSE;
  for (const gchar *p = uri + 14; *p; p++) if (!g_ascii_isalnum (*p)) return FALSE;
  SpotifyConnectTrack *track = g_new0 (SpotifyConnectTrack, 1);
  track->uri = g_strdup (uri);
  track->uid = g_strdup (command_string (object, "uid"));
  const gchar *provider = command_string (object, "provider");
  track->provider = g_strdup (provider ? provider : "context");
  g_ptr_array_add (tracks, track);
  return TRUE;
}

static gboolean
command_tracks (GPtrArray *tracks, JsonObject *object, const gchar *key)
{
  JsonNode *node = object ? json_object_get_member (object, key) : NULL;
  if (!node) return TRUE;
  if (!JSON_NODE_HOLDS_ARRAY (node)) return FALSE;
  JsonArray *array = json_node_get_array (node);
  if (json_array_get_length (array) > 10000 || tracks->len + json_array_get_length (array) > 10000) return FALSE;
  for (guint i = 0; i < json_array_get_length (array); i++) {
    JsonNode *item = json_array_get_element (array, i);
    if (!JSON_NODE_HOLDS_OBJECT (item) || !command_track (tracks, json_node_get_object (item))) return FALSE;
  }
  return TRUE;
}

void
spotifygtk_connect_command_free (SpotifyConnectCommand *command)
{
  if (!command) return;
  g_free (command->endpoint); g_free (command->context_uri); g_free (command->uri);
  g_free (command->track_uid); g_free (command->queue_revision);
  g_ptr_array_unref (command->next); g_ptr_array_unref (command->previous); g_free (command);
}

SpotifyConnectCommand *
spotifygtk_connect_parse_command (const gchar *json, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  if (!json || strlen (json) > 8 * 1024 * 1024) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Missing/oversized Connect command");
    return NULL;
  }
  if (!json_parser_load_from_data (parser, json, -1, error)) return NULL;
  JsonNode *root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Connect command is not an object");
    return NULL;
  }
  JsonObject *object = json_node_get_object (root);
  SpotifyConnectCommand *cmd = g_new0 (SpotifyConnectCommand, 1);
  cmd->next = g_ptr_array_new_with_free_func (connect_track_free);
  cmd->previous = g_ptr_array_new_with_free_func (connect_track_free);
  cmd->endpoint = g_strdup (command_string (object, "endpoint"));
  cmd->queue_revision = g_strdup (command_string (object, "queue_revision"));
  cmd->seek_ms = command_number (object, "value", command_number (object, "position", -1));
  cmd->track_index = -1;
  cmd->shuffle_mode = command_bool (object, "shuffling_context");
  cmd->repeating_context = command_bool (object, "repeating_context");
  cmd->repeating_track = command_bool (object, "repeating_track");
  if (g_strcmp0 (cmd->endpoint, "set_shuffling_context") == 0) cmd->shuffle_mode = command_bool (object, "value");
  if (g_strcmp0 (cmd->endpoint, "set_repeating_context") == 0) cmd->repeating_context = command_bool (object, "value");
  if (g_strcmp0 (cmd->endpoint, "set_repeating_track") == 0) cmd->repeating_track = command_bool (object, "value");
  gboolean valid = cmd->endpoint != NULL;
  JsonObject *track = command_object (object, "track");
  if (track) {
    valid &= command_track (cmd->next, track);
    cmd->uri = g_strdup (command_string (track, "uri"));
    cmd->track_uid = g_strdup (command_string (track, "uid"));
  }
  valid &= command_tracks (cmd->next, object, "next_tracks");
  valid &= command_tracks (cmd->previous, object, "prev_tracks");
  JsonObject *context = command_object (object, "context");
  if (context) {
    cmd->context_uri = g_strdup (command_string (context, "uri"));
    JsonNode *pages = json_object_get_member (context, "pages");
    if (pages && !JSON_NODE_HOLDS_ARRAY (pages)) valid = FALSE;
    JsonArray *array = pages && JSON_NODE_HOLDS_ARRAY (pages) ? json_node_get_array (pages) : NULL;
    for (guint i = 0; array && i < json_array_get_length (array); i++) {
      JsonNode *page = json_array_get_element (array, i);
      valid &= JSON_NODE_HOLDS_OBJECT (page) && command_tracks (cmd->next, json_node_get_object (page), "tracks");
    }
  }
  JsonObject *options = command_object (object, "options");
  if (options) {
    JsonObject *skip = command_object (options, "skip_to");
    if (skip) {
      g_free (cmd->uri); cmd->uri = g_strdup (command_string (skip, "track_uri"));
      g_free (cmd->track_uid); cmd->track_uid = g_strdup (command_string (skip, "track_uid"));
      cmd->track_index = CLAMP (command_number (skip, "track_index", -1), -1, 9999);
    }
    cmd->seek_ms = command_number (options, "seek_to", cmd->seek_ms);
    cmd->paused = command_bool (options, "initially_paused") == 1;
    JsonObject *override = command_object (options, "player_options_override");
    if (override) {
      cmd->shuffle_mode = command_bool (override, "shuffling_context");
      cmd->repeating_context = command_bool (override, "repeating_context");
      cmd->repeating_track = command_bool (override, "repeating_track");
    }
  }
  JsonObject *modes = command_object (object, "modes");
  if (g_strcmp0 (command_string (modes, "context_enhancement"), "RECOMMENDATION") == 0) cmd->shuffle_mode = 2;
  if (cmd->context_uri && (!g_str_has_prefix (cmd->context_uri, "spotify:") || strlen (cmd->context_uri) > 1024)) valid = FALSE;
  /* An omitted queue is not an explicit empty queue. Never erase playback
   * state for a malformed command that lacks its required payload. */
  if (g_strcmp0 (cmd->endpoint, "set_queue") == 0 && !json_object_has_member (object, "next_tracks")) valid = FALSE;
  if (g_strcmp0 (cmd->endpoint, "add_to_queue") == 0 && cmd->next->len == 0) valid = FALSE;
  if (!valid) {
    spotifygtk_connect_command_free (cmd);
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Malformed Connect command/track list");
    return NULL;
  }
  return cmd;
}

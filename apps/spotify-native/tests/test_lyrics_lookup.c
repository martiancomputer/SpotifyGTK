#include <glib.h>
#include <json-glib/json-glib.h>
#include "ui/lyrics_lookup.h"

/* Synthetic metadata and text only: no downloaded lyrics or account data. */
static JsonObject *
add_recording (JsonArray *results, const gchar *title, const gchar *artist,
               const gchar *album, gdouble duration, gboolean synced)
{
  JsonObject *object = json_object_new ();
  json_object_set_string_member (object, "trackName", title);
  json_object_set_string_member (object, "artistName", artist);
  json_object_set_string_member (object, "albumName", album);
  json_object_set_double_member (object, "duration", duration);
  json_object_set_string_member (object, synced ? "syncedLyrics" : "plainLyrics",
                                 synced ? "[00:01.00] fixture" : "fixture");
  json_array_add_object_element (results, object);
  return object;
}

static JsonObject *
select_match (JsonArray *results)
{
  return spotifygtk_lyrics_select_match (results, "Example Song",
    "Example Artist, Guest Artist", "Example Album", 330000);
}

static void
test_featured_credits (void)
{
  const gchar *titles[] = {
    "Example Song", "Example Song (feat. Guest Artist)",
    "Example Song [ft. Guest Artist]", "Example Song featuring Guest Artist  ",
    "Example Song"
  };
  const gchar *artists[] = {
    "Example Artist feat. Guest Artist", "Example Artist", "Example Artist",
    "Example Artist", "Example Artist, Guest Artist"
  };
  for (guint i = 0; i < G_N_ELEMENTS (titles); i++) {
    g_autoptr(JsonArray) results = json_array_new ();
    JsonObject *record = add_recording (results, titles[i], artists[i],
                                       "Example Album", 330.057, TRUE);
    g_assert_true (select_match (results) == record);
  }
}

static void
test_reject_different_recordings (void)
{
  const gchar *versions[] = {
    "Example Song (Live)", "Example Song (Remix)",
    "Example Song (Radio Edit)", "Example Song (Acoustic)",
    "Example Song (feat. Guest Artist) [Original Mix]"
  };
  g_autoptr(JsonArray) results = json_array_new ();
  for (guint i = 0; i < G_N_ELEMENTS (versions); i++)
    add_recording (results, versions[i], "Example Artist feat. Guest Artist",
                   "Example Album", 330, TRUE);
  add_recording (results, "Example Song", "Example Artist Junior feat. Guest Artist",
                 "Example Album", 330, TRUE);
  add_recording (results, "Example Song", "Example Artist feat. Wrong Guest",
                 "Example Album", 330, TRUE);
  add_recording (results, "Example Song", "Example Artist",
                 "Example Album", 330, TRUE);
  add_recording (results, "Example Song", "Example Artist feat. Guest Artist",
                 "Other Album", 330, TRUE);
  add_recording (results, "Example Song", "Example Artist feat. Guest Artist",
                 "Example Album", 332.001, TRUE);
  g_assert_null (select_match (results));
}

static void
test_ranking_and_duration (void)
{
  g_autoptr(JsonArray) results = json_array_new ();
  JsonObject *plain = add_recording (results, "Example Song",
    "Example Artist feat. Guest Artist", "Example Album", 330, FALSE);
  g_assert_true (select_match (results) == plain);
  JsonObject *boundary = add_recording (results, "Example Song",
    "Example Artist feat. Guest Artist", "Example Album", 332, TRUE);
  g_assert_true (select_match (results) == boundary);
  JsonObject *closer = add_recording (results, "Example Song",
    "Example Artist feat. Guest Artist", "Example Album", 330.057, TRUE);
  g_assert_true (select_match (results) == closer);
  JsonObject *integer = add_recording (results, "Example Song",
    "Example Artist feat. Guest Artist", "Example Album", 330, TRUE);
  json_object_set_int_member (integer, "duration", 330);
  g_assert_true (select_match (results) == integer);
}

static void
test_normalization (void)
{
  g_autoptr(JsonArray) results = json_array_new ();
  JsonObject *record = add_recording (results, "EXAMPLE—Song",
    "Éxample Artist feat. Guest Artist", "Example: Album", 330, TRUE);
  g_assert_true (spotifygtk_lyrics_select_match (results, "Example Song",
    "E\314\201xample Artist, Guest Artist", "example album", 330000) == record);
  /* Feature normalization must work in either source's title. */
  g_assert_true (spotifygtk_lyrics_select_match (results,
    "Example Song (feat. Guest Artist)", "Éxample Artist, Guest Artist",
    "Example Album", 330000) == record);
}

static void
test_malformed_results (void)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser,
    "[null, 7, [], {}, {\"trackName\":7,\"artistName\":false,"
    "\"albumName\":[],\"duration\":\"330\",\"syncedLyrics\":{}}]", -1, NULL));
  JsonArray *results = json_node_get_array (json_parser_get_root (parser));
  g_assert_null (select_match (results));
  JsonObject *record = add_recording (results, "Example Song",
    "Example Artist feat. Guest Artist", "Example Album", 330, TRUE);
  json_object_set_boolean_member (record, "syncedLyrics", TRUE);
  g_assert_null (select_match (results));
  json_object_set_null_member (record, "syncedLyrics");
  g_assert_null (select_match (results));
  json_object_set_string_member (record, "plainLyrics", "");
  g_assert_null (select_match (results));
  json_object_set_string_member (record, "plainLyrics", "fixture");
  json_object_set_string_member (record, "duration", "330");
  g_assert_null (select_match (results));
  json_object_set_double_member (record, "duration", 0);
  g_assert_null (select_match (results));
}

static void
test_invalid_query (void)
{
  g_autoptr(JsonArray) results = json_array_new ();
  add_recording (results, "Example Song", "Example Artist feat. Guest Artist",
                 "Example Album", 330, TRUE);
  g_assert_null (spotifygtk_lyrics_select_match (NULL, "Example Song",
    "Example Artist", "Example Album", 330000));
  g_assert_null (spotifygtk_lyrics_select_match (results, "---",
    "Example Artist", "Example Album", 330000));
  g_assert_null (spotifygtk_lyrics_select_match (results, "Example Song",
    NULL, "Example Album", 330000));
  g_assert_null (spotifygtk_lyrics_select_match (results, "Example Song",
    "Example Artist", "Example Album", 0));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/lyrics/featured-credits", test_featured_credits);
  g_test_add_func ("/lyrics/different-recordings", test_reject_different_recordings);
  g_test_add_func ("/lyrics/ranking-duration", test_ranking_and_duration);
  g_test_add_func ("/lyrics/normalization", test_normalization);
  g_test_add_func ("/lyrics/malformed-results", test_malformed_results);
  g_test_add_func ("/lyrics/invalid-query", test_invalid_query);
  return g_test_run ();
}

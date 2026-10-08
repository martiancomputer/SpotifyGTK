/* Synthetic display metadata only: no captured feed or account identifiers. */
#include "spotify/home_feed.h"
#include "spotify/image_uri.h"
#include <string.h>

#define ID "0000000000000000000000"
static const gchar fixture[] =
  "{\"data\":{\"home\":{\"__typename\":\"HomeResponsePayload\","
  "\"greeting\":{\"transformedLabel\":\"Hello fixture\"},"
  "\"sectionContainer\":{\"sections\":{\"items\":["
  "{\"data\":{\"__typename\":\"HomeShortsSectionData\"},\"sectionItems\":{\"items\":["
  "{\"uri\":\"spotify:playlist:" ID "\",\"content\":{\"data\":{\"__typename\":\"Playlist\","
  "\"name\":\"Fixture playlist\",\"images\":{\"items\":[{\"sources\":[{\"url\":\"https://mosaic.scdn.co/fixture\"}]}]}}}},"
  "{\"uri\":\"spotify:collection:tracks\",\"content\":{\"data\":{\"__typename\":\"UnknownType\"}}}]}},"
  "{\"data\":{\"__typename\":\"HomeRecentlyPlayedSectionData\"},\"sectionItems\":{\"items\":["
  "{\"content\":{\"data\":{\"__typename\":\"List\",\"items\":{\"items\":[{\"entity\":{"
  "\"_uri\":\"spotify:album:" ID "\",\"data\":{\"__typename\":\"Entity\","
  "\"identityTrait\":{\"name\":\"Recent fixture\",\"contributors\":{\"items\":[{\"name\":\"Artist fixture\"}]}},"
  "\"visualIdentityTrait\":{\"squareCoverImage\":{\"image\":{\"data\":{\"sources\":["
  "{\"url\":\"https://i.scdn.co/image/abcdef\",\"maxWidth\":300}]}}}}}}}]}}}}]}},"
  "{\"data\":{\"__typename\":\"HomeGenericSectionData\",\"title\":{\"transformedLabel\":\"Made for fixture\"}},"
  "\"sectionItems\":{\"items\":[{\"content\":{\"data\":{\"__typename\":\"Album\","
  "\"uri\":\"spotify:album:" ID "\",\"name\":\"Album fixture\","
  "\"artists\":{\"items\":[{\"profile\":{\"name\":\"Artist fixture\"}}]},"
  "\"coverArt\":{\"sources\":[{\"url\":\"https://i.scdn.co/image/aaa\",\"width\":640},"
  "{\"url\":\"https://i.scdn.co/image/bbb\",\"width\":300},"
  "{\"url\":\"https://i.scdn.co/image/ccc\",\"width\":64}]}}}}]}},"
  "{\"data\":{\"__typename\":\"HomePromotionSectionData\"},\"sectionItems\":{\"items\":[]}}"
  "]}}}}}";

static JsonNode *
root_new (void)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GError) error = NULL;
  g_assert_true (json_parser_load_from_data (parser, fixture, -1, &error));
  g_assert_no_error (error);
  return json_node_copy (json_parser_get_root (parser));
}

static JsonArray *
sections_of (JsonNode *root)
{
  JsonObject *home = json_object_get_object_member (
    json_object_get_object_member (json_node_get_object (root), "data"), "home");
  return json_object_get_array_member (json_object_get_object_member (
    json_object_get_object_member (home, "sectionContainer"), "sections"), "items");
}

static void
test_feed (void)
{
  g_autoptr(JsonNode) root = root_new ();
  g_autoptr(GError) error = NULL;
  g_autoptr(SpotifyHomeFeed) feed = spotifygtk_home_feed_parse (root, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (feed->sections->len, ==, 3);
  SpotifyHomeSection *s = g_ptr_array_index (feed->sections, 0);
  g_assert_cmpstr (s->title, ==, "Continue listening");
  g_assert_cmpuint (s->cards->len, ==, 2);
  SpotifyHomeCard *c = g_ptr_array_index (s->cards, 0);
  g_assert_cmpstr (c->title, ==, "Fixture playlist");
  g_assert_cmpstr (c->cover, ==, "https://mosaic.scdn.co/fixture");
  s = g_ptr_array_index (feed->sections, 1);
  c = g_ptr_array_index (s->cards, 0);
  g_assert_cmpstr (s->title, ==, "Recently played");
  g_assert_cmpstr (c->title, ==, "Recent fixture");
  g_assert_cmpstr (c->subtitle, ==, "Artist fixture");
  g_assert_cmpstr (c->cover, ==, "abcdef");
  s = g_ptr_array_index (feed->sections, 2);
  c = g_ptr_array_index (s->cards, 0);
  g_assert_cmpstr (c->cover, ==, "bbb");
  g_autoptr(GBytes) bytes = spotifygtk_home_feed_encode (feed);
  g_autoptr(SpotifyHomeFeed) cached = spotifygtk_home_feed_decode (bytes);
  g_assert_nonnull (cached);
  g_assert_cmpuint (cached->sections->len, ==, 3);
  g_autoptr(GBytes) second = spotifygtk_home_feed_encode (cached);
  g_assert_true (g_bytes_equal (bytes, second));
  g_assert_cmpuint (g_bytes_get_size (bytes), <, 4096);
}

static void
test_bounds (void)
{
  g_autoptr(JsonNode) root = root_new ();
  JsonArray *sections = sections_of (root);
  JsonNode *section = json_array_get_element (sections, 2);
  JsonArray *cards = json_object_get_array_member (json_object_get_object_member (
    json_node_get_object (section), "sectionItems"), "items");
  JsonNode *card = json_array_get_element (cards, 0);
  for (guint i = 0; i < 100; i++) json_array_add_element (cards, json_node_copy (card));
  for (guint i = 0; i < 100; i++) json_array_add_element (sections, json_node_copy (section));
  g_autoptr(SpotifyHomeFeed) feed = spotifygtk_home_feed_parse (root, NULL);
  g_assert_cmpuint (feed->sections->len, ==, SPOTIFYGTK_HOME_MAX_SECTIONS);
  for (guint i = 0; i < feed->sections->len; i++)
    g_assert_cmpuint (((SpotifyHomeSection *) g_ptr_array_index (feed->sections, i))->cards->len,
                     <=, SPOTIFYGTK_HOME_MAX_CARDS);
  g_autoptr(GBytes) bytes = spotifygtk_home_feed_encode (feed);
  g_assert_cmpuint (g_bytes_get_size (bytes), <, 1024 * 1024);
}

static void
test_long_snapshot (void)
{
  g_autofree gchar *name = g_strnfill (512, 'n');
  g_autofree gchar *subtitle = g_strnfill (2048, 's');
  g_autofree gchar *path = g_strnfill (1900, 'a');
  g_autofree gchar *cover = g_strconcat ("https://mosaic.scdn.co/", path, NULL);
  SpotifyHomeCard card = { "spotify:album:" ID, name, subtitle, cover };
  g_autoptr(GPtrArray) cards = g_ptr_array_new ();
  for (guint i = 0; i < SPOTIFYGTK_HOME_MAX_CARDS; i++) g_ptr_array_add (cards, &card);
  SpotifyHomeSection section = { name, "", cards };
  g_autoptr(GPtrArray) sections = g_ptr_array_new ();
  for (guint i = 0; i < SPOTIFYGTK_HOME_MAX_SECTIONS; i++) g_ptr_array_add (sections, &section);
  SpotifyHomeFeed feed = { "", sections };
  g_autoptr(GBytes) bytes = spotifygtk_home_feed_encode (&feed);
  g_assert_cmpuint (g_bytes_get_size (bytes), >, 1024 * 1024);
  g_autoptr(SpotifyHomeFeed) decoded = spotifygtk_home_feed_decode (bytes);
  g_assert_nonnull (decoded);
  g_assert_cmpuint (decoded->sections->len, ==, SPOTIFYGTK_HOME_MAX_SECTIONS);
}

static void
test_validation (void)
{
  const gchar *bad[] = { "local:track:fixture", "https://example.com", "spotify:album:short",
    "spotify:playlist:000000000000000000000/", "spotify:user::collection" };
  for (guint i = 0; i < G_N_ELEMENTS (bad); i++) g_assert_false (spotifygtk_home_uri_supported (bad[i]));
  g_assert_true (spotifygtk_home_uri_supported ("spotify:artist:" ID));
  const gchar *urls[] = { "http://i.scdn.co/image/a", "https://i.scdn.co.example.com/a",
    "https://localhost/a", "https://127.0.0.1/a", "file:///tmp/a",
    "https://user@i.scdn.co/a", "https://i.scdn.co:123/a", "https://i.scdn.co/a#fragment" };
  for (guint i = 0; i < G_N_ELEMENTS (urls); i++) g_assert_false (spotifygtk_image_uri_allowed (urls[i]));
  g_assert_true (spotifygtk_image_uri_allowed ("https://pickasso.spotifycdn.com/fixture"));
  g_autoptr(GBytes) garbage = g_bytes_new_static ("bad cache", 9);
  g_assert_null (spotifygtk_home_feed_decode (garbage));
  g_autoptr(JsonNode) root = root_new ();
  JsonArray *sections = sections_of (root);
  while (json_array_get_length (sections)) json_array_remove_element (sections, 0);
  g_autoptr(GError) error = NULL;
  g_assert_null (spotifygtk_home_feed_parse (root, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/home/feed-and-cache", test_feed);
  g_test_add_func ("/home/response-bounds", test_bounds);
  g_test_add_func ("/home/validation", test_validation);
  g_test_add_func ("/home/long-snapshot", test_long_snapshot);
  return g_test_run ();
}

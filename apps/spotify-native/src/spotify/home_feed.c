#include "home_feed.h"
#include "image_uri.h"
#include <string.h>

static JsonObject *
obj (JsonObject *parent, const gchar *key)
{
  JsonNode *n = parent ? json_object_get_member (parent, key) : NULL;
  return n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

static JsonArray *
arr (JsonObject *parent, const gchar *key)
{
  JsonNode *n = parent ? json_object_get_member (parent, key) : NULL;
  return n && JSON_NODE_HOLDS_ARRAY (n) ? json_node_get_array (n) : NULL;
}

static JsonObject *
item (JsonArray *array, guint index)
{
  JsonNode *n = array && index < json_array_get_length (array)
    ? json_array_get_element (array, index) : NULL;
  return n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

static const gchar *
str (JsonObject *parent, const gchar *key)
{
  JsonNode *n = parent ? json_object_get_member (parent, key) : NULL;
  const gchar *s = n && JSON_NODE_HOLDS_VALUE (n) &&
    json_node_get_value_type (n) == G_TYPE_STRING ? json_node_get_string (n) : NULL;
  return s && strlen (s) <= 2048 && g_utf8_validate (s, -1, NULL) ? s : "";
}

gboolean
spotifygtk_home_uri_supported (const gchar *uri)
{
  if (!uri || strlen (uri) > 256) return FALSE;
  const gchar *prefixes[] = { "spotify:album:", "spotify:playlist:",
                              "spotify:artist:", "spotify:track:" };
  for (guint i = 0; i < G_N_ELEMENTS (prefixes); i++) {
    if (!g_str_has_prefix (uri, prefixes[i])) continue;
    const gchar *id = uri + strlen (prefixes[i]);
    if (strlen (id) != 22) return FALSE;
    for (; *id; id++) if (!g_ascii_isalnum (*id)) return FALSE;
    return TRUE;
  }
  if (g_str_equal (uri, "spotify:collection:tracks")) return TRUE;
  if (g_str_has_prefix (uri, "spotify:user:") && g_str_has_suffix (uri, ":collection")) {
    const gchar *p = uri + strlen ("spotify:user:");
    const gchar *end = uri + strlen (uri) - strlen (":collection");
    if (p == end) return FALSE;
    for (; p < end; p++)
      if (!(g_ascii_isalnum (*p) || *p == '-' || *p == '_' || *p == '.' || *p == '%'))
        return FALSE;
    return TRUE;
  }
  return FALSE;
}

static void
card_free (SpotifyHomeCard *c)
{
  g_free (c->uri); g_free (c->title); g_free (c->subtitle); g_free (c->cover); g_free (c);
}

static void
section_free (SpotifyHomeSection *s)
{
  g_free (s->title); g_free (s->subtitle); g_ptr_array_unref (s->cards); g_free (s);
}

void
spotifygtk_home_feed_free (SpotifyHomeFeed *f)
{
  if (!f) return;
  g_free (f->greeting); g_ptr_array_unref (f->sections); g_free (f);
}

static SpotifyHomeFeed *
feed_new (const gchar *greeting)
{
  SpotifyHomeFeed *f = g_new0 (SpotifyHomeFeed, 1);
  f->greeting = g_strdup (greeting && strlen (greeting) <= 512 ? greeting : "");
  f->sections = g_ptr_array_new_with_free_func ((GDestroyNotify) section_free);
  return f;
}

static gchar *
cover_from_sources (JsonArray *sources)
{
  const gchar *best = NULL;
  gint64 width = -1;
  for (guint i = 0; sources && i < MIN (json_array_get_length (sources), 16); i++) {
    JsonObject *source = item (sources, i);
    const gchar *url = str (source, "url");
    if (!spotifygtk_image_uri_allowed (url)) continue;
    JsonNode *n = source ? json_object_get_member (source, "width") : NULL;
    if (!n) n = source ? json_object_get_member (source, "maxWidth") : NULL;
    gint64 w = n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_INT64
      ? json_node_get_int (n) : 0;
    /* Shelf-sized art, not a multi-megapixel hero, whenever available. */
    if (!best || (w >= 180 && (width < 180 || w < width))) { best = url; width = w; }
  }
  if (!best) return g_strdup ("");
  const gchar *prefix = "https://i.scdn.co/image/";
  if (g_str_has_prefix (best, prefix)) {
    const gchar *id = best + strlen (prefix);
    gboolean hex = *id != '\0';
    for (const gchar *p = id; *p; p++) if (!g_ascii_isxdigit (*p)) hex = FALSE;
    if (hex) return g_strdup (id);
  }
  return g_strdup (best);
}

static gchar *
contributors (JsonArray *items, gboolean profiles)
{
  GString *names = g_string_new (NULL);
  for (guint i = 0; items && i < MIN (json_array_get_length (items), 6); i++) {
    JsonObject *o = item (items, i);
    if (obj (o, "data")) o = obj (o, "data");
    const gchar *name = str (profiles ? obj (o, "profile") : o, "name");
    if (!*name || strlen (name) > 256) continue;
    if (names->len) g_string_append (names, ", ");
    g_string_append (names, name);
  }
  return g_string_free (names, FALSE);
}

static SpotifyHomeCard *
parse_card (JsonObject *o, const gchar *fallback_uri)
{
  const gchar *kind = str (o, "__typename");
  const gchar *uri = str (o, "uri");
  if (!*uri) uri = fallback_uri ?: "";
  if (!spotifygtk_home_uri_supported (uri)) return NULL;
  const gchar *name = str (o, "name");
  JsonArray *sources = NULL;
  gchar *subtitle = NULL;
  if (g_str_equal (kind, "Album")) {
    sources = arr (obj (o, "coverArt"), "sources");
    subtitle = contributors (arr (obj (o, "artists"), "items"), TRUE);
  } else if (g_str_equal (kind, "Playlist")) {
    sources = arr (item (arr (obj (o, "images"), "items"), 0), "sources");
    subtitle = g_strdup ("Playlist");
  } else if (g_str_equal (kind, "Artist")) {
    name = str (obj (o, "profile"), "name");
    sources = arr (obj (obj (o, "visuals"), "avatarImage"), "sources");
    subtitle = g_strdup ("Artist");
  } else if (g_str_equal (kind, "Track")) {
    sources = arr (obj (obj (o, "albumOfTrack"), "coverArt"), "sources");
    subtitle = contributors (arr (obj (o, "artists"), "items"), TRUE);
  } else if (g_str_equal (kind, "Entity")) {
    name = str (obj (o, "identityTrait"), "name");
    subtitle = contributors (arr (obj (obj (o, "identityTrait"), "contributors"), "items"), FALSE);
    sources = arr (obj (obj (obj (obj (o, "visualIdentityTrait"), "squareCoverImage"), "image"), "data"), "sources");
  } else if (g_str_equal (kind, "UnknownType") &&
             (g_str_equal (uri, "spotify:collection:tracks") || g_str_has_suffix (uri, ":collection"))) {
    name = "Liked Songs"; subtitle = g_strdup ("Collection");
  } else return NULL;
  if (!*name || strlen (name) > 512) { g_free (subtitle); return NULL; }
  SpotifyHomeCard *c = g_new0 (SpotifyHomeCard, 1);
  c->uri = g_strdup (uri); c->title = g_strdup (name);
  c->subtitle = subtitle ?: g_strdup (""); c->cover = cover_from_sources (sources);
  return c;
}

SpotifyHomeFeed *
spotifygtk_home_feed_parse (JsonNode *root, GError **error)
{
  JsonObject *r = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonObject *home = obj (obj (r, "data"), "home");
  if (g_strcmp0 (str (home, "__typename"), "HomeResponsePayload") != 0) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Home feed unavailable");
    return NULL;
  }
  SpotifyHomeFeed *f = feed_new (str (obj (home, "greeting"), "transformedLabel"));
  JsonArray *sections = arr (obj (obj (home, "sectionContainer"), "sections"), "items");
  for (guint i = 0; sections && i < MIN (json_array_get_length (sections), 128) &&
       f->sections->len < SPOTIFYGTK_HOME_MAX_SECTIONS; i++) {
    JsonObject *section = item (sections, i), *data = obj (section, "data");
    const gchar *kind = str (data, "__typename");
    gboolean recent = g_str_equal (kind, "HomeRecentlyPlayedSectionData");
    gboolean shortcuts = g_str_equal (kind, "HomeShortsSectionData");
    if (!(recent || shortcuts || g_str_equal (kind, "HomeGenericSectionData") ||
          g_str_equal (kind, "HomeGridSectionData") ||
          g_str_equal (kind, "HomeFeedBaselineSectionData"))) continue;
    SpotifyHomeSection *s = g_new0 (SpotifyHomeSection, 1);
    const gchar *title = str (obj (data, "title"), "transformedLabel");
    s->title = g_strdup (*title && strlen (title) <= 512 ? title : recent ? "Recently played" :
                         shortcuts ? "Continue listening" : "More for you");
    const gchar *subtitle = str (obj (data, "subtitle"), "transformedLabel");
    s->subtitle = g_strdup (strlen (subtitle) <= 1024 ? subtitle : "");
    s->cards = g_ptr_array_new_with_free_func ((GDestroyNotify) card_free);
    JsonArray *items = arr (obj (section, "sectionItems"), "items");
    for (guint j = 0; items && j < MIN (json_array_get_length (items), 64) &&
         s->cards->len < SPOTIFYGTK_HOME_MAX_CARDS; j++) {
      JsonObject *entry = item (items, j), *content = obj (entry, "content"), *value = obj (content, "data");
      if (g_str_equal (str (value, "__typename"), "List")) {
        JsonArray *entities = arr (obj (value, "items"), "items");
        for (guint k = 0; entities && k < MIN (json_array_get_length (entities), 64) &&
             s->cards->len < SPOTIFYGTK_HOME_MAX_CARDS; k++) {
          JsonObject *entity = obj (item (entities, k), "entity");
          SpotifyHomeCard *c = parse_card (obj (entity, "data"), str (entity, "_uri"));
          if (c) g_ptr_array_add (s->cards, c);
        }
      } else {
        SpotifyHomeCard *c = parse_card (value ?: content, str (entry, "uri"));
        if (c) g_ptr_array_add (s->cards, c);
      }
    }
    if (s->cards->len) g_ptr_array_add (f->sections, s);
    else section_free (s);
  }
  if (!f->sections->len) {
    spotifygtk_home_feed_free (f);
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Home returned no supported music");
    return NULL;
  }
  return f;
}

GBytes *
spotifygtk_home_feed_encode (const SpotifyHomeFeed *f)
{
  GVariantBuilder sections;
  g_variant_builder_init (&sections, G_VARIANT_TYPE ("a(ssa(ssss))"));
  for (guint i = 0; i < MIN (f->sections->len, SPOTIFYGTK_HOME_MAX_SECTIONS); i++) {
    SpotifyHomeSection *s = g_ptr_array_index (f->sections, i);
    GVariantBuilder cards;
    g_variant_builder_init (&cards, G_VARIANT_TYPE ("a(ssss)"));
    for (guint j = 0; j < MIN (s->cards->len, SPOTIFYGTK_HOME_MAX_CARDS); j++) {
      SpotifyHomeCard *c = g_ptr_array_index (s->cards, j);
      g_variant_builder_add (&cards, "(ssss)", c->uri, c->title, c->subtitle, c->cover);
    }
    g_variant_builder_add (&sections, "(ss@a(ssss))", s->title, s->subtitle, g_variant_builder_end (&cards));
  }
  g_autoptr(GVariant) v = g_variant_ref_sink (g_variant_new ("(s@a(ssa(ssss)))",
    f->greeting, g_variant_builder_end (&sections)));
  return g_variant_get_data_as_bytes (v);
}

SpotifyHomeFeed *
spotifygtk_home_feed_decode (GBytes *bytes)
{
  /* All 320 bounded cards can legitimately exceed 1 MiB with long CDN URLs
   * and contributor names. Two MiB still exceeds the worst valid snapshot. */
  if (!bytes || g_bytes_get_size (bytes) > 2 * 1024 * 1024) return NULL;
  g_autoptr(GVariant) v = g_variant_ref_sink (g_variant_new_from_bytes (
    G_VARIANT_TYPE ("(sa(ssa(ssss)))"), bytes, FALSE));
  if (!g_variant_is_normal_form (v)) return NULL;
  const gchar *greeting;
  g_autoptr(GVariant) sections = NULL;
  g_variant_get (v, "(&s@a(ssa(ssss)))", &greeting, &sections);
  if (strlen (greeting) > 512 || g_variant_n_children (sections) > SPOTIFYGTK_HOME_MAX_SECTIONS) return NULL;
  SpotifyHomeFeed *f = feed_new (greeting);
  for (gsize i = 0; i < g_variant_n_children (sections); i++) {
    g_autoptr(GVariant) section = g_variant_get_child_value (sections, i);
    g_autoptr(GVariant) cards = NULL;
    const gchar *title, *subtitle;
    g_variant_get (section, "(&s&s@a(ssss))", &title, &subtitle, &cards);
    if (strlen (title) > 512 || strlen (subtitle) > 1024 ||
        !g_variant_n_children (cards) ||
        g_variant_n_children (cards) > SPOTIFYGTK_HOME_MAX_CARDS) goto invalid;
    SpotifyHomeSection *s = g_new0 (SpotifyHomeSection, 1);
    s->title = g_strdup (title); s->subtitle = g_strdup (subtitle);
    s->cards = g_ptr_array_new_with_free_func ((GDestroyNotify) card_free);
    g_ptr_array_add (f->sections, s);
    for (gsize j = 0; j < g_variant_n_children (cards); j++) {
      const gchar *uri, *name, *sub, *cover;
      g_variant_get_child (cards, j, "(&s&s&s&s)", &uri, &name, &sub, &cover);
      if (!spotifygtk_home_uri_supported (uri) || !*name || strlen (name) > 512 ||
          strlen (sub) > 2048 || strlen (cover) > 2048) goto invalid;
      if (*cover && !spotifygtk_image_uri_allowed (cover))
        for (const gchar *p = cover; *p; p++) if (!g_ascii_isxdigit (*p)) goto invalid;
      SpotifyHomeCard *c = g_new0 (SpotifyHomeCard, 1);
      c->uri = g_strdup (uri); c->title = g_strdup (name);
      c->subtitle = g_strdup (sub); c->cover = g_strdup (cover);
      g_ptr_array_add (s->cards, c);
    }
  }
  if (f->sections->len) return f;
invalid:
  spotifygtk_home_feed_free (f);
  return NULL;
}

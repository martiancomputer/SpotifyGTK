#include "lyrics_lookup.h"
#include <math.h>
#include <string.h>

static const gchar *
text_member (JsonObject *object, const gchar *name)
{
  JsonNode *node = json_object_get_member (object, name);
  return node && JSON_NODE_HOLDS_VALUE (node) &&
    json_node_get_value_type (node) == G_TYPE_STRING
      ? json_node_get_string (node) : NULL;
}

static gchar *
match_key (const gchar *text)
{
  g_autofree gchar *folded = g_utf8_casefold (text ? text : "", -1);
  g_autofree gchar *normal = g_utf8_normalize (folded, -1, G_NORMALIZE_ALL_COMPOSE);
  GString *key = g_string_new (NULL);
  gboolean space = FALSE;
  for (const gchar *p = normal; p && *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    if (!g_unichar_isalnum (c)) {
      space = key->len > 0;
      continue;
    }
    if (space) g_string_append_c (key, ' ');
    g_string_append_unichar (key, c);
    space = FALSE;
  }
  return g_string_free (key, FALSE);
}

static gchar *
title_key (const gchar *title)
{
  /* Strip only a trailing featured-artist credit, never version information
   * such as Remix, Live, Radio Edit, Acoustic or Instrumental. */
  static gsize feature_regex;
  if (g_once_init_enter (&feature_regex)) {
    GRegex *regex = g_regex_new (
      "\\s+(?:\\((?:feat\\.?|ft\\.?|featuring)\\s+[^()]+\\)|"
      "\\[(?:feat\\.?|ft\\.?|featuring)\\s+[^\\[\\]]+\\]|"
      "(?:feat\\.?|ft\\.?|featuring)\\s+[^()\\[\\]]+)$",
      G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
    g_once_init_leave (&feature_regex, (gsize) regex);
  }
  g_autofree gchar *trimmed = g_strdup (title ? title : "");
  g_strstrip (trimmed);
  g_autofree gchar *base = g_regex_replace ((GRegex *) feature_regex,
    trimmed, -1, 0, "", 0, NULL);
  return match_key (base);
}

static gboolean
artists_match (const gchar *artists, const gchar *candidate, const gchar *title)
{
  if (!candidate || !*candidate) return FALSE;
  g_auto(GStrv) credits = g_strsplit (artists ? artists : "", ", ", -1);
  g_autofree gchar *primary = match_key (credits[0]);
  g_autofree gchar *found = match_key (candidate);
  g_autofree gchar *all_credits = match_key (artists);
  if (!*primary) return FALSE;
  if (!g_str_equal (primary, found) && !g_str_equal (all_credits, found)) {
    if (!g_str_has_prefix (found, primary)) return FALSE;
    const gchar *suffix = found + strlen (primary);
    if (!(g_str_has_prefix (suffix, " feat ") ||
          g_str_has_prefix (suffix, " ft ") ||
          g_str_has_prefix (suffix, " featuring ")))
      return FALSE;
  }
  /* When Spotify supplies collaborators, require their credits too, whether
   * LRCLIB puts them in artistName or in the title's feature suffix. */
  g_autofree gchar *full = g_strconcat (candidate, " ", title ? title : "", NULL);
  g_autofree gchar *full_key = match_key (full);
  g_autofree gchar *haystack = g_strdup_printf (" %s ", full_key);
  for (guint i = 1; credits[i]; i++) {
    g_autofree gchar *credit = match_key (credits[i]);
    if (!*credit) continue;
    g_autofree gchar *needle = g_strdup_printf (" %s ", credit);
    if (!strstr (haystack, needle)) return FALSE;
  }
  return TRUE;
}

JsonObject *
spotifygtk_lyrics_select_match (JsonArray *results, const gchar *title,
                                const gchar *artists, const gchar *album,
                                gint64 duration_ms)
{
  if (!results || !title || !*title || !artists || !*artists || duration_ms <= 0)
    return NULL;
  g_autofree gchar *wanted_title = title_key (title);
  g_autofree gchar *wanted_album = match_key (album);
  if (!*wanted_title) return NULL;
  JsonObject *best = NULL;
  gboolean best_synced = FALSE;
  gdouble best_delta = G_MAXDOUBLE;
  guint count = json_array_get_length (results);
  if (count > 1000) return NULL;
  for (guint i = 0; i < count; i++) {
    JsonNode *node = json_array_get_element (results, i);
    if (!node || !JSON_NODE_HOLDS_OBJECT (node)) continue;
    JsonObject *object = json_node_get_object (node);
    const gchar *name = text_member (object, "trackName");
    const gchar *artist = text_member (object, "artistName");
    const gchar *record = text_member (object, "albumName");
    const gchar *synced = text_member (object, "syncedLyrics");
    const gchar *plain = text_member (object, "plainLyrics");
    JsonNode *duration = json_object_get_member (object, "duration");
    if (!duration || !JSON_NODE_HOLDS_VALUE (duration) ||
        (json_node_get_value_type (duration) != G_TYPE_DOUBLE &&
         json_node_get_value_type (duration) != G_TYPE_INT64)) continue;
    gdouble seconds = json_node_get_double (duration);
    gdouble delta = fabs (seconds - duration_ms / 1000.0);
    if (!isfinite (seconds) || seconds <= 0 || delta > 2.0 ||
        ((!synced || !*synced) && (!plain || !*plain))) continue;
    g_autofree gchar *name_key = title_key (name);
    g_autofree gchar *record_key = match_key (record);
    if (!g_str_equal (wanted_title, name_key) ||
        (*wanted_album && !g_str_equal (wanted_album, record_key)) ||
        !artists_match (artists, artist, name)) continue;
    gboolean has_synced = synced && *synced;
    if (!best || (has_synced && !best_synced) ||
        (has_synced == best_synced && delta < best_delta)) {
      best = object;
      best_synced = has_synced;
      best_delta = delta;
    }
  }
  return best;
}

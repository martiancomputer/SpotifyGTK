#include "catalog_snapshot.h"
#include <string.h>

#define LEGACY_TRACK_TYPE "(ssssxbssssi)"
#define LEGACY_TRACKS_TYPE "a" LEGACY_TRACK_TYPE
#define LEGACY_RELEASES_TYPE "a(sssii" LEGACY_TRACKS_TYPE ")"
#define TRACK_TYPE "(ssssxbssssia(ss))"
#define TRACKS_TYPE "a" TRACK_TYPE
#define RELEASE_TYPE "(sssii" TRACKS_TYPE ")"
#define RELEASES_TYPE "a" RELEASE_TYPE
#define TEXT(s) ((s) ? (s) : "")
#define LIBRARY_TYPE "a(sssiii)"

/* Keep old offline snapshots readable while storing structured credits in new
 * snapshots. A prefix disambiguates the two serialized GVariant schemas. */
static GBytes *snapshot_bytes (GVariant *value)
{
  GByteArray *bytes = g_byte_array_new ();
  g_byte_array_append (bytes, (const guint8 *) "SGC2", 4);
  g_byte_array_append (bytes, g_variant_get_data (value), g_variant_get_size (value));
  return g_byte_array_free_to_bytes (bytes);
}

static GVariant *snapshot_value (GBytes *bytes, const gchar *type,
                                  const gchar *legacy_type)
{
  gsize size;
  const gchar *data = g_bytes_get_data (bytes, &size);
  gboolean modern = size >= 4 && memcmp (data, "SGC2", 4) == 0;
  g_autoptr(GBytes) payload = modern ? g_bytes_new_from_bytes (bytes, 4, size - 4)
                                    : g_bytes_ref (bytes);
  return g_variant_ref_sink (g_variant_new_from_bytes (
    G_VARIANT_TYPE (modern ? type : legacy_type), payload, FALSE));
}

static GVariant *pack_tracks (GPtrArray *tracks)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE (TRACKS_TYPE));
  for (guint i = 0; tracks && i < tracks->len; i++) {
    const SpotifyNativeTrack *t = tracks->pdata[i];
    g_variant_builder_add (&b, "(ssssxbssssi@a(ss))", TEXT (t->uri), TEXT (t->name),
      TEXT (t->artists), TEXT (t->album), t->duration_ms, t->is_explicit,
      TEXT (t->cover_id), TEXT (t->cover_id_small), TEXT (t->album_uri),
      TEXT (t->artist_uri), t->release_year, t->artist_credits
        ? t->artist_credits
        : g_variant_new_array (G_VARIANT_TYPE ("(ss)"), NULL, 0));
  }
  return g_variant_builder_end (&b);
}
static GPtrArray *unpack_tracks (GVariant *value)
{
  if (g_variant_n_children (value) > 20000) return NULL;
  GPtrArray *tracks = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_track_free);
  for (gsize i = 0; i < g_variant_n_children (value); i++) {
    g_autoptr(GVariant) item = g_variant_get_child_value (value, i);
    const gchar *uri, *name, *artists, *album, *cover, *small, *album_uri, *artist_uri;
    SpotifyNativeTrack *t = g_new0 (SpotifyNativeTrack, 1);
    if (g_variant_is_of_type (item, G_VARIANT_TYPE (TRACK_TYPE)))
      g_variant_get (item, "(&s&s&s&sxb&s&s&s&si@a(ss))", &uri, &name, &artists,
        &album, &t->duration_ms, &t->is_explicit, &cover, &small,
        &album_uri, &artist_uri, &t->release_year, &t->artist_credits);
    else
      g_variant_get (item, "(&s&s&s&sxb&s&s&s&si)", &uri, &name, &artists,
        &album, &t->duration_ms, &t->is_explicit, &cover, &small,
        &album_uri, &artist_uri, &t->release_year);
    if (!g_str_has_prefix (uri, "spotify:track:")) {
      spotifygtk_native_track_free (t);
      g_ptr_array_unref (tracks);
      return NULL;
    }
    if (t->artist_credits) {
      /* A child variant otherwise keeps the entire catalogue's serialized
       * buffer alive when only one playing track survives. Detach its small
       * credit block; row and queue copies can then share that block alone. */
      g_autoptr(GBytes) credits = g_bytes_new (g_variant_get_data (t->artist_credits),
                                              g_variant_get_size (t->artist_credits));
      g_clear_pointer (&t->artist_credits, g_variant_unref);
      t->artist_credits = g_variant_ref_sink (g_variant_new_from_bytes (
        G_VARIANT_TYPE ("a(ss)"), credits, TRUE));
    }
    t->uri = g_strdup (uri); t->name = g_strdup (name);
    t->artists = g_strdup (artists); t->album = g_strdup (album);
    t->cover_id = *cover ? g_strdup (cover) : NULL;
    t->cover_id_small = *small ? g_strdup (small) : NULL;
    t->album_uri = *album_uri ? g_strdup (album_uri) : NULL;
    t->artist_uri = *artist_uri ? g_strdup (artist_uri) : NULL;
    g_ptr_array_add (tracks, t);
  }
  return tracks;
}
GBytes *spotifygtk_catalog_tracks_pack (GPtrArray *tracks)
{
  g_autoptr(GVariant) value = g_variant_ref_sink (pack_tracks (tracks));
  return snapshot_bytes (value);
}
GPtrArray *spotifygtk_catalog_tracks_unpack (GBytes *bytes)
{
  if (!bytes) return NULL;
  g_autoptr(GVariant) value = snapshot_value (bytes, TRACKS_TYPE, LEGACY_TRACKS_TYPE);
  return g_variant_is_normal_form (value) ? unpack_tracks (value) : NULL;
}
static gboolean
snapshot_contains_track (GHashTable *membership, const SpotifyNativeTrack *track)
{
  return track && track->uri && g_hash_table_contains (membership, track->uri);
}

guint
spotifygtk_catalog_tracks_filter (GPtrArray *tracks, GHashTable *membership)
{
  if (!tracks || !membership) return 0;
  guint dropped = 0;
  for (guint i = tracks->len; i > 0;) {
    if (snapshot_contains_track (membership, tracks->pdata[i - 1])) {
      i--;
      continue;
    }
    guint end = i;
    do { i--; } while (i > 0 && !snapshot_contains_track (membership, tracks->pdata[i - 1]));
    /* Remove stale runs from the end, preserving order and avoiding a splice
     * per track when a completed empty collection replaces a large snapshot. */
    g_ptr_array_remove_range (tracks, i, end - i);
    dropped += end - i;
  }
  return dropped;
}

GBytes *spotifygtk_catalog_releases_pack (GPtrArray *releases)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE (RELEASES_TYPE));
  for (guint i = 0; releases && i < releases->len; i++) {
    const SpotifyNativeRelease *r = releases->pdata[i];
    g_variant_builder_add (&b, "(sssii@" TRACKS_TYPE ")",
      TEXT (r->uri), TEXT (r->name), TEXT (r->cover_id), r->year,
      (gint) r->type, pack_tracks (r->tracks));
  }
  g_autoptr(GVariant) value = g_variant_ref_sink (g_variant_builder_end (&b));
  return snapshot_bytes (value);
}
GPtrArray *spotifygtk_catalog_releases_unpack (GBytes *bytes)
{
  if (!bytes) return NULL;
  g_autoptr(GVariant) value = snapshot_value (bytes, RELEASES_TYPE, LEGACY_RELEASES_TYPE);
  if (!g_variant_is_normal_form (value) || g_variant_n_children (value) > 2000)
    return NULL;
  GPtrArray *releases = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_release_free);
  guint total = 0;
  for (gsize i = 0; i < g_variant_n_children (value); i++) {
    g_autoptr(GVariant) item = g_variant_get_child_value (value, i);
    const gchar *uri, *name, *cover;
    g_autoptr(GVariant) tracks = NULL;
    SpotifyNativeRelease *r = g_new0 (SpotifyNativeRelease, 1);
    gint type;
    g_variant_get (item, g_variant_is_of_type (value, G_VARIANT_TYPE (RELEASES_TYPE))
      ? "(&s&s&sii@" TRACKS_TYPE ")" : "(&s&s&sii@" LEGACY_TRACKS_TYPE ")",
      &uri, &name, &cover, &r->year, &type, &tracks);
    r->uri = g_strdup (uri); r->name = g_strdup (name);
    r->cover_id = *cover ? g_strdup (cover) : NULL; r->type = type;
    total += g_variant_n_children (tracks);
    if (!g_str_has_prefix (uri, "spotify:album:") || total > 20000 ||
        !(r->tracks = unpack_tracks (tracks))) {
      spotifygtk_native_release_free (r);
      g_ptr_array_unref (releases);
      return NULL;
    }
    g_ptr_array_add (releases, r);
  }
  return releases;
}

GBytes *
spotifygtk_catalog_library_pack (GPtrArray *releases, GHashTable *dates)
{
  if (!releases || releases->len > 20000) return NULL;
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE (LIBRARY_TYPE));
  for (guint i = 0; i < releases->len; i++) {
    const SpotifyNativeRelease *r = releases->pdata[i];
    g_variant_builder_add (&builder, "(sssiii)", TEXT (r->uri), TEXT (r->name),
      TEXT (r->cover_id), r->year, (gint) r->type,
      dates ? GPOINTER_TO_INT (g_hash_table_lookup (dates, r->uri)) : 0);
  }
  g_autoptr(GVariant) value = g_variant_ref_sink (g_variant_builder_end (&builder));
  if (g_variant_get_size (value) > 4 * 1024 * 1024 - 4) return NULL;
  GByteArray *out = g_byte_array_new ();
  g_byte_array_append (out, (const guint8 *) "SGL1", 4);
  g_byte_array_append (out, g_variant_get_data (value), g_variant_get_size (value));
  return g_byte_array_free_to_bytes (out);
}

GPtrArray *
spotifygtk_catalog_library_unpack (GBytes *bytes, GHashTable **dates)
{
  *dates = NULL;
  if (!bytes || g_bytes_get_size (bytes) < 4 ||
      g_bytes_get_size (bytes) > 4 * 1024 * 1024 ||
      memcmp (g_bytes_get_data (bytes, NULL), "SGL1", 4) != 0) return NULL;
  g_autoptr(GBytes) payload = g_bytes_new_from_bytes (bytes, 4, g_bytes_get_size (bytes) - 4);
  g_autoptr(GVariant) value = g_variant_ref_sink (g_variant_new_from_bytes (
    G_VARIANT_TYPE (LIBRARY_TYPE), payload, FALSE));
  if (!g_variant_is_normal_form (value) || g_variant_n_children (value) > 20000)
    return NULL;
  g_autoptr(GPtrArray) releases = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_release_free);
  g_autoptr(GHashTable) added = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GVariantIter iter;
  const gchar *uri, *name, *cover;
  gint year, type, date;
  g_variant_iter_init (&iter, value);
  while (g_variant_iter_loop (&iter, "(&s&s&siii)", &uri, &name, &cover, &year, &type, &date)) {
    if (!g_str_has_prefix (uri, "spotify:album:") || strlen (uri) > 128 ||
        g_hash_table_contains (added, uri) || date < 0 ||
        type < SPOTIFY_ALBUM_TYPE_UNKNOWN || type > SPOTIFY_ALBUM_TYPE_EP)
      return NULL;
    SpotifyNativeRelease *r = g_new0 (SpotifyNativeRelease, 1);
    r->uri = g_strdup (uri); r->name = g_strdup (name);
    r->cover_id = *cover ? g_strdup (cover) : NULL;
    r->year = year; r->type = type;
    r->tracks = g_ptr_array_new_with_free_func ((GDestroyNotify) spotifygtk_native_track_free);
    g_ptr_array_add (releases, r);
    g_hash_table_insert (added, g_strdup (uri), GINT_TO_POINTER (date));
  }
  *dates = g_steal_pointer (&added);
  return g_steal_pointer (&releases);
}

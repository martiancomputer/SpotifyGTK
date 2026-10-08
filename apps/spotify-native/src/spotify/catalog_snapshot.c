#include "catalog_snapshot.h"

#define TRACK_TYPE "(ssssxbssssi)"
#define TRACKS_TYPE "a" TRACK_TYPE
#define RELEASE_TYPE "(sssii" TRACKS_TYPE ")"
#define RELEASES_TYPE "a" RELEASE_TYPE
#define TEXT(s) ((s) ? (s) : "")

static GVariant *pack_tracks (GPtrArray *tracks)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE (TRACKS_TYPE));
  for (guint i = 0; tracks && i < tracks->len; i++) {
    const SpotifyNativeTrack *t = tracks->pdata[i];
    g_variant_builder_add (&b, TRACK_TYPE, TEXT (t->uri), TEXT (t->name),
      TEXT (t->artists), TEXT (t->album), t->duration_ms, t->is_explicit,
      TEXT (t->cover_id), TEXT (t->cover_id_small), TEXT (t->album_uri),
      TEXT (t->artist_uri), t->release_year);
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
    g_variant_get (item, "(&s&s&s&sxb&s&s&s&si)", &uri, &name, &artists,
      &album, &t->duration_ms, &t->is_explicit, &cover, &small,
      &album_uri, &artist_uri, &t->release_year);
    if (!g_str_has_prefix (uri, "spotify:track:")) {
      spotifygtk_native_track_free (t);
      g_ptr_array_unref (tracks);
      return NULL;
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
  return g_variant_get_data_as_bytes (value);
}
GPtrArray *spotifygtk_catalog_tracks_unpack (GBytes *bytes)
{
  if (!bytes) return NULL;
  g_autoptr(GVariant) value = g_variant_ref_sink (
    g_variant_new_from_bytes (G_VARIANT_TYPE (TRACKS_TYPE), bytes, FALSE));
  return g_variant_is_normal_form (value) ? unpack_tracks (value) : NULL;
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
  return g_variant_get_data_as_bytes (value);
}
GPtrArray *spotifygtk_catalog_releases_unpack (GBytes *bytes)
{
  if (!bytes) return NULL;
  g_autoptr(GVariant) value = g_variant_ref_sink (
    g_variant_new_from_bytes (G_VARIANT_TYPE (RELEASES_TYPE), bytes, FALSE));
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
    g_variant_get (item, "(&s&s&sii@" TRACKS_TYPE ")",
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

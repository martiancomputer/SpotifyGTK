#include "config.h"
#include "local_catalog.h"
#include "settings.h"

#include <glib/gstdio.h>
#include <string.h>

#if HAVE_LOCAL_AV
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#endif

#define LOCAL_MAX_FILES 100000u
#define LOCAL_MAX_DEPTH 64u
#define LOCAL_MAX_ART_BYTES (8u * 1024u * 1024u)
#define LOCAL_INDEX_MAX_BYTES (64u * 1024u * 1024u)
#define LOCAL_INDEX_MAGIC "SGTKLOCAL1\n"

struct _SpotifyGtkLocalSnapshot {
  gatomicrefcount refs;
  GPtrArray *tracks;     /* SpotifyGtkLocalTrack*, owned */
  GPtrArray *albums;     /* SpotifyGtkLocalAlbum*, owned */
  GHashTable *by_track;  /* URI -> borrowed local track */
  GHashTable *by_album;  /* URI -> borrowed album */
  GHashTable *art_paths; /* cover ID -> owned path */
  GPtrArray *directories; /* discovered directories, owned strings */
};

struct _SpotifyGtkLocalCatalog {
  GObject parent_instance;
  SpotifyGtkLocalSnapshot *snapshot;
  GPtrArray *monitors; /* GFileMonitor*, owned */
  GCancellable *scan_cancel;
  gboolean scan_active;
  guint generation;
  guint rescan_id;
  GHashTable *dirty_dirs; /* owned path strings, main thread */
  GHashTable *art_dirs;   /* directories whose sidecar art changed */
};

G_DEFINE_FINAL_TYPE (SpotifyGtkLocalCatalog, spotifygtk_local_catalog, G_TYPE_OBJECT)

static SpotifyGtkLocalCatalog *global_catalog;
static GMutex snapshot_lock;
static guint changed_signal;

static void
local_track_free (gpointer data)
{
  SpotifyGtkLocalTrack *track = data;
  spotifygtk_native_track_free (track->display);
  g_free (track->path);
  g_free (track->album_artist);
  g_free (track->codec);
  g_free (track->art_path);
  g_free (track);
}

static void
local_album_free (gpointer data)
{
  SpotifyGtkLocalAlbum *album = data;
  g_free (album->uri);
  g_free (album->title);
  g_free (album->artist);
  g_free (album->cover_id);
  g_ptr_array_unref (album->tracks);
  g_free (album);
}

static SpotifyGtkLocalSnapshot *
snapshot_new (void)
{
  SpotifyGtkLocalSnapshot *s = g_new0 (SpotifyGtkLocalSnapshot, 1);
  g_atomic_ref_count_init (&s->refs);
  s->tracks = g_ptr_array_new_with_free_func (local_track_free);
  s->albums = g_ptr_array_new_with_free_func (local_album_free);
  s->by_track = g_hash_table_new (g_str_hash, g_str_equal);
  s->by_album = g_hash_table_new (g_str_hash, g_str_equal);
  s->art_paths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  s->directories = g_ptr_array_new_with_free_func (g_free);
  return s;
}

SpotifyGtkLocalSnapshot *
spotifygtk_local_snapshot_ref (SpotifyGtkLocalSnapshot *s)
{
  if (s) g_atomic_ref_count_inc (&s->refs);
  return s;
}

void
spotifygtk_local_snapshot_unref (SpotifyGtkLocalSnapshot *s)
{
  if (!s || !g_atomic_ref_count_dec (&s->refs)) return;
  g_hash_table_unref (s->by_track);
  g_hash_table_unref (s->by_album);
  g_hash_table_unref (s->art_paths);
  g_ptr_array_unref (s->albums);
  g_ptr_array_unref (s->tracks);
  g_ptr_array_unref (s->directories);
  g_free (s);
}

SpotifyGtkLocalSnapshot *
spotifygtk_local_catalog_ref_snapshot (void)
{
  g_mutex_lock (&snapshot_lock);
  SpotifyGtkLocalSnapshot *s = global_catalog
    ? spotifygtk_local_snapshot_ref (global_catalog->snapshot) : NULL;
  g_mutex_unlock (&snapshot_lock);
  return s;
}

const GPtrArray *
spotifygtk_local_snapshot_albums (SpotifyGtkLocalSnapshot *s)
{
  return s ? s->albums : NULL;
}

const SpotifyGtkLocalAlbum *
spotifygtk_local_snapshot_find_album (SpotifyGtkLocalSnapshot *s,
                                      const gchar *uri)
{
  return s && uri ? g_hash_table_lookup (s->by_album, uri) : NULL;
}

gchar *
spotifygtk_local_catalog_dup_track_path (const gchar *uri)
{
  if (!uri || !g_str_has_prefix (uri, "local:track:")) return NULL;
  g_mutex_lock (&snapshot_lock);
  SpotifyGtkLocalTrack *t = global_catalog && global_catalog->snapshot
    ? g_hash_table_lookup (global_catalog->snapshot->by_track, uri) : NULL;
  gchar *path = t ? g_strdup (t->path) : NULL;
  g_mutex_unlock (&snapshot_lock);
  return path;
}

gchar *
spotifygtk_local_catalog_dup_art_path (const gchar *cover_id)
{
  if (!cover_id || !g_str_has_prefix (cover_id, "local:")) return NULL;
  g_mutex_lock (&snapshot_lock);
  const gchar *path = global_catalog && global_catalog->snapshot
    ? g_hash_table_lookup (global_catalog->snapshot->art_paths, cover_id) : NULL;
  gchar *copy = g_strdup (path);
  g_mutex_unlock (&snapshot_lock);
  return copy;
}

static gchar *
hashed_uri (const gchar *prefix, const gchar *key)
{
  g_autofree gchar *digest = g_compute_checksum_for_string (
    G_CHECKSUM_SHA256, key, -1);
  return g_strconcat (prefix, digest, NULL);
}

static gchar *
normal_key (const gchar *text)
{
  g_autofree gchar *valid = g_utf8_make_valid (text ? text : "", -1);
  g_autofree gchar *folded = g_utf8_casefold (valid, -1);
  return g_utf8_normalize (folded, -1, G_NORMALIZE_ALL_COMPOSE);
}

#if HAVE_LOCAL_AV
static gchar *
metadata_string (AVDictionary *tags, const gchar *key)
{
  AVDictionaryEntry *tag = av_dict_get (tags, key, NULL, 0);
  if (!tag || !tag->value) return NULL;
  gsize n = strnlen (tag->value, 4097);
  if (n == 0 || n > 4096 || !g_utf8_validate (tag->value, n, NULL)) return NULL;
  return g_strndup (tag->value, n);
}

static gchar *
metadata_string_fallback (AVDictionary *primary, AVDictionary *fallback,
                          const gchar *key)
{
  gchar *value = metadata_string (primary, key);
  return value ? value : metadata_string (fallback, key);
}

static gint
metadata_number (AVDictionary *tags, const gchar *key, gint limit)
{
  AVDictionaryEntry *tag = av_dict_get (tags, key, NULL, 0);
  if (!tag || !tag->value) return 0;
  gchar *end = NULL;
  gint64 n = g_ascii_strtoll (tag->value, &end, 10);
  return end != tag->value && n > 0 && n <= limit ? (gint) n : 0;
}

static gint
metadata_number_fallback (AVDictionary *primary, AVDictionary *fallback,
                          const gchar *key, gint limit)
{
  gint value = metadata_number (primary, key, limit);
  return value ? value : metadata_number (fallback, key, limit);
}

static gchar *
sidecar_art (const gchar *directory)
{
  static const gchar *names[] = {
    "cover.jpg", "cover.jpeg", "cover.png", "folder.jpg", "folder.png"
  };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
    g_autofree gchar *path = g_build_filename (directory, names[i], NULL);
    GStatBuf st;
    if (g_stat (path, &st) == 0 && S_ISREG (st.st_mode) &&
        st.st_size > 0 && st.st_size <= LOCAL_MAX_ART_BYTES)
      return g_steal_pointer (&path);
  }
  return NULL;
}

static gboolean
cached_art_matches (const SpotifyGtkLocalTrack *track)
{
  g_autofree gchar *directory = g_path_get_dirname (track->path);
  g_autofree gchar *preferred = sidecar_art (directory);
  if (preferred && g_strcmp0 (preferred, track->art_path) != 0)
    return FALSE;
  if (!track->art_path)
    return preferred == NULL;
  g_autoptr(GFile) art = g_file_new_for_path (track->art_path);
  g_autoptr(GFileInfo) info = g_file_query_info (
    art, G_FILE_ATTRIBUTE_STANDARD_SIZE ","
         G_FILE_ATTRIBUTE_TIME_MODIFIED ","
         G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
  if (!info || g_file_info_get_size (info) <= 0 ||
      g_file_info_get_size (info) > LOCAL_MAX_ART_BYTES)
    return FALSE;
  gint64 mtime = (gint64) g_file_info_get_attribute_uint64 (
    info, G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC +
    g_file_info_get_attribute_uint32 (
      info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
  g_autofree gchar *key = g_strdup_printf ("%s:%" G_GINT64_FORMAT,
                                           track->art_path, mtime);
  g_autofree gchar *expected = hashed_uri ("local:", key);
  return g_strcmp0 (expected, track->display->cover_id) == 0;
}

static SpotifyGtkLocalTrack *
probe_track (SpotifyGtkLocalSnapshot *s, const gchar *path,
             guint64 size, gint64 mtime_us)
{
  AVFormatContext *format = NULL;
  AVDictionary *opts = NULL;
  av_dict_set (&opts, "probesize", "1048576", 0);
  av_dict_set (&opts, "analyzeduration", "2000000", 0);
  int opened = avformat_open_input (&format, path, NULL, &opts);
  av_dict_free (&opts);
  if (opened < 0 || !format) return NULL;
  if (avformat_find_stream_info (format, NULL) < 0) {
    avformat_close_input (&format);
    return NULL;
  }
  int stream_idx = av_find_best_stream (format, AVMEDIA_TYPE_AUDIO, -1, -1,
                                        NULL, 0);
  if (stream_idx < 0) {
    avformat_close_input (&format);
    return NULL;
  }
  AVStream *stream = format->streams[stream_idx];
  AVCodecParameters *codec = stream->codecpar;
  const gchar *extension = strrchr (path, '.');
  const gchar *codec_name = codec ? avcodec_get_name (codec->codec_id) : NULL;
  if (!codec || codec->sample_rate < 8000 || codec->sample_rate > 384000 ||
      codec->ch_layout.nb_channels < 1 || codec->ch_layout.nb_channels > 8 ||
      !extension || !codec_name ||
      !((g_ascii_strcasecmp (extension, ".flac") == 0 &&
         codec->codec_id == AV_CODEC_ID_FLAC) ||
        (g_ascii_strcasecmp (extension, ".opus") == 0 &&
         codec->codec_id == AV_CODEC_ID_OPUS) ||
        (g_ascii_strcasecmp (extension, ".wav") == 0 &&
         g_str_has_prefix (codec_name, "pcm_")) ||
        ((g_ascii_strcasecmp (extension, ".m4a") == 0 ||
          g_ascii_strcasecmp (extension, ".aac") == 0) &&
         codec->codec_id == AV_CODEC_ID_AAC))) {
    avformat_close_input (&format);
    return NULL;
  }

  AVDictionary *tags = stream->metadata;
  AVDictionary *fallback = format->metadata;
  g_autofree gchar *directory = g_path_get_dirname (path);
  g_autofree gchar *base = g_path_get_basename (path);
  gchar *dot = strrchr (base, '.');
  if (dot) *dot = '\0';
  SpotifyGtkLocalTrack *t = g_new0 (SpotifyGtkLocalTrack, 1);
  t->display = g_new0 (SpotifyNativeTrack, 1);
  t->path = g_strdup (path);
  t->codec = g_strdup (codec_name);
  t->file_size = size;
  t->mtime_us = mtime_us;
  t->display->uri = hashed_uri ("local:track:", path);
  t->display->name = metadata_string_fallback (tags, fallback, "title");
  if (!t->display->name) t->display->name = g_utf8_make_valid (base, -1);
  t->display->artists = metadata_string_fallback (tags, fallback, "artist");
  if (!t->display->artists) t->display->artists = g_strdup ("Unknown artist");
  t->album_artist = metadata_string_fallback (tags, fallback, "album_artist");
  if (!t->album_artist) t->album_artist = g_strdup (t->display->artists);
  t->display->album = metadata_string_fallback (tags, fallback, "album");
  if (!t->display->album) {
    g_autofree gchar *dirname = g_path_get_basename (directory);
    t->display->album = g_utf8_make_valid (dirname, -1);
  }
  t->track_number = metadata_number_fallback (tags, fallback, "track", 9999);
  t->disc_number = metadata_number_fallback (tags, fallback, "disc", 999);
  t->display->release_year = metadata_number_fallback (tags, fallback, "date", 9999);
  if (!t->display->release_year)
    t->display->release_year = metadata_number_fallback (tags, fallback, "year", 9999);
  gint64 duration = stream->duration > 0
    ? av_rescale_q (stream->duration, stream->time_base,
                    (AVRational) { 1, 1000 })
    : format->duration > 0 ? format->duration / 1000 : 0;
  if (duration > 0 && duration < G_GINT64_CONSTANT (604800000))
    t->display->duration_ms = duration;
  t->sample_rate = codec->sample_rate;
  t->channels = codec->ch_layout.nb_channels;
  t->source_bits = codec->bits_per_raw_sample > 0
    ? codec->bits_per_raw_sample : codec->bits_per_coded_sample;

  gchar *art_path = sidecar_art (directory);
  if (!art_path) {
    for (unsigned i = 0; i < format->nb_streams; i++) {
      AVStream *art = format->streams[i];
      if (!(art->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
          art->attached_pic.size <= 0 ||
          (guint) art->attached_pic.size > LOCAL_MAX_ART_BYTES) continue;
      g_autofree gchar *source_key = g_strdup_printf (
        "%s:%" G_GINT64_FORMAT, path, mtime_us);
      g_autofree gchar *id = hashed_uri ("local:", source_key);
      g_autofree gchar *cache_dir = g_build_filename (
        g_get_user_cache_dir (), "spotifygtk", "local-art", NULL);
      if (g_mkdir_with_parents (cache_dir, 0700) != 0) break;
      art_path = g_build_filename (cache_dir, id + strlen ("local:"), NULL);
      if (!g_file_test (art_path, G_FILE_TEST_IS_REGULAR) &&
          !g_file_set_contents_full (art_path,
            (const gchar *) art->attached_pic.data, art->attached_pic.size,
            G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL)) {
        g_clear_pointer (&art_path, g_free);
        continue;
      }
      break;
    }
  }
  if (art_path) {
    g_autoptr(GFile) art_file = g_file_new_for_path (art_path);
    g_autoptr(GFileInfo) art_info = g_file_query_info (
      art_file, G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
      G_FILE_QUERY_INFO_NONE, NULL, NULL);
    gint64 art_mtime = art_info
      ? (gint64) g_file_info_get_attribute_uint64 (
          art_info, G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC +
        g_file_info_get_attribute_uint32 (
          art_info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC)
      : mtime_us;
    g_autofree gchar *art_key = g_strdup_printf ("%s:%" G_GINT64_FORMAT,
                                                art_path, art_mtime);
    t->display->cover_id = hashed_uri ("local:", art_key);
    t->display->cover_id_small = g_strdup (t->display->cover_id);
    t->art_path = g_strdup (art_path);
    g_hash_table_replace (s->art_paths, g_strdup (t->display->cover_id),
                          art_path);
  }
  avformat_close_input (&format);
  g_autoptr(GFile) source = g_file_new_for_path (path);
  g_autoptr(GFileInfo) current = g_file_query_info (
    source, G_FILE_ATTRIBUTE_STANDARD_SIZE ","
            G_FILE_ATTRIBUTE_TIME_MODIFIED ","
            G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
  gint64 current_mtime = current
    ? (gint64) g_file_info_get_attribute_uint64 (
        current, G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC +
      g_file_info_get_attribute_uint32 (
        current, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC)
    : -1;
  if (!current || g_file_info_get_size (current) < 0 ||
      (guint64) g_file_info_get_size (current) != size ||
      current_mtime != mtime_us) {
    if (t->display->cover_id)
      g_hash_table_remove (s->art_paths, t->display->cover_id);
    local_track_free (t);
    return NULL;
  }
  return t;
}
#endif

static gboolean
supported_name (const gchar *name)
{
  if (!name) return FALSE;
  const gchar *dot = strrchr (name, '.');
  if (!dot) return FALSE;
  return g_ascii_strcasecmp (dot, ".flac") == 0 ||
         g_ascii_strcasecmp (dot, ".opus") == 0 ||
         g_ascii_strcasecmp (dot, ".wav") == 0 ||
         g_ascii_strcasecmp (dot, ".m4a") == 0 ||
         g_ascii_strcasecmp (dot, ".aac") == 0;
}

static gboolean
path_in_directory (const gchar *path, const gchar *directory)
{
  gsize n = strlen (directory);
  return g_str_has_prefix (path, directory) &&
    (directory[n - 1] == G_DIR_SEPARATOR ||
     path[n] == G_DIR_SEPARATOR || path[n] == '\0');
}

static gboolean
path_in_any_directory (const gchar *path, GPtrArray *directories)
{
  for (guint i = 0; directories && i < directories->len; i++)
    if (path_in_directory (path, g_ptr_array_index (directories, i)))
      return TRUE;
  return FALSE;
}

static gchar *
index_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), "spotifygtk",
                           "local-index-v1", NULL);
}

static gchar *
index_string (GKeyFile *keyfile, const gchar *group, const gchar *name,
              gsize max_len)
{
  gchar *value = g_key_file_get_string (keyfile, group, name, NULL);
  if (!value) return NULL;
  if (strlen (value) > max_len || !g_utf8_validate (value, -1, NULL)) {
    g_free (value);
    return NULL;
  }
  return value;
}

/* This cache is plain metadata, never GTK objects or artwork bytes. Keys
 * borrow each track's path; the table destroys values after comparison. */
static GHashTable *
load_index (void)
{
  GHashTable *tracks = g_hash_table_new_full (
    g_str_hash, g_str_equal, NULL, local_track_free);
  g_autofree gchar *path = index_path ();
  GStatBuf st;
  if (g_stat (path, &st) != 0 || st.st_size <= 0 ||
      st.st_size > LOCAL_INDEX_MAX_BYTES) return tracks;
  g_autofree gchar *data = NULL;
  gsize length = 0;
  if (!g_file_get_contents (path, &data, &length, NULL) ||
      length > LOCAL_INDEX_MAX_BYTES ||
      !g_str_has_prefix (data, LOCAL_INDEX_MAGIC)) return tracks;
  const gchar *checksum = data + strlen (LOCAL_INDEX_MAGIC);
  if (length < strlen (LOCAL_INDEX_MAGIC) + 65 || checksum[64] != '\n')
    return tracks;
  const gchar *payload = checksum + 65;
  g_autofree gchar *actual = g_compute_checksum_for_data (
    G_CHECKSUM_SHA256, (const guchar *) payload,
    length - (payload - data));
  if (strncmp (checksum, actual, 64) != 0) return tracks;
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (keyfile, payload, length - (payload - data),
                                  G_KEY_FILE_NONE, NULL)) return tracks;
  gsize n_groups = 0;
  g_auto(GStrv) groups = g_key_file_get_groups (keyfile, &n_groups);
  if (n_groups > LOCAL_MAX_FILES) return tracks;
  for (gsize i = 0; i < n_groups; i++) {
    const gchar *group = groups[i];
    if (!g_str_has_prefix (group, "track-")) continue;
    SpotifyGtkLocalTrack *track = g_new0 (SpotifyGtkLocalTrack, 1);
    track->display = g_new0 (SpotifyNativeTrack, 1);
    track->path = index_string (keyfile, group, "path", 8192);
    track->display->name = index_string (keyfile, group, "title", 4096);
    track->display->artists = index_string (keyfile, group, "artist", 4096);
    track->display->album = index_string (keyfile, group, "album", 4096);
    track->album_artist = index_string (keyfile, group, "album_artist", 4096);
    track->codec = index_string (keyfile, group, "codec", 32);
    track->art_path = index_string (keyfile, group, "art_path", 8192);
    track->display->cover_id = index_string (keyfile, group, "cover_id", 80);
    track->file_size = g_key_file_get_uint64 (keyfile, group, "size", NULL);
    track->mtime_us = g_key_file_get_int64 (keyfile, group, "mtime", NULL);
    track->display->duration_ms = g_key_file_get_int64 (
      keyfile, group, "duration", NULL);
    track->display->release_year = g_key_file_get_integer (
      keyfile, group, "year", NULL);
    track->track_number = g_key_file_get_integer (keyfile, group, "track", NULL);
    track->disc_number = g_key_file_get_integer (keyfile, group, "disc", NULL);
    track->sample_rate = g_key_file_get_integer (keyfile, group, "rate", NULL);
    track->channels = g_key_file_get_integer (keyfile, group, "channels", NULL);
    track->source_bits = g_key_file_get_integer (keyfile, group, "bits", NULL);
    if (!track->path || !g_path_is_absolute (track->path) ||
        !track->display->name || !track->display->artists ||
        !track->display->album || !track->album_artist || !track->codec ||
        !track->file_size || track->mtime_us <= 0 ||
        track->sample_rate < 8000 || track->sample_rate > 384000 ||
        track->channels < 1 || track->channels > 8 ||
        track->display->duration_ms < 0 ||
        track->display->duration_ms > 604800000) {
      local_track_free (track);
      continue;
    }
    if (track->art_path && (!g_path_is_absolute (track->art_path) ||
                            !track->display->cover_id)) {
      g_clear_pointer (&track->art_path, g_free);
      g_clear_pointer (&track->display->cover_id, g_free);
    }
    if (track->display->cover_id)
      track->display->cover_id_small = g_strdup (track->display->cover_id);
    track->display->uri = hashed_uri ("local:track:", track->path);
    g_hash_table_replace (tracks, track->path, track);
  }
  return tracks;
}

static void
save_index (SpotifyGtkLocalSnapshot *snapshot)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  for (guint i = 0; i < snapshot->tracks->len; i++) {
    const SpotifyGtkLocalTrack *track = g_ptr_array_index (snapshot->tracks, i);
    g_autofree gchar *group = g_strdup_printf ("track-%u", i);
    g_key_file_set_string (keyfile, group, "path", track->path);
    g_key_file_set_string (keyfile, group, "title", track->display->name);
    g_key_file_set_string (keyfile, group, "artist", track->display->artists);
    g_key_file_set_string (keyfile, group, "album", track->display->album);
    g_key_file_set_string (keyfile, group, "album_artist", track->album_artist);
    g_key_file_set_string (keyfile, group, "codec", track->codec);
    if (track->art_path && track->display->cover_id) {
      g_key_file_set_string (keyfile, group, "art_path", track->art_path);
      g_key_file_set_string (keyfile, group, "cover_id",
                             track->display->cover_id);
    }
    g_key_file_set_uint64 (keyfile, group, "size", track->file_size);
    g_key_file_set_int64 (keyfile, group, "mtime", track->mtime_us);
    g_key_file_set_int64 (keyfile, group, "duration",
                          track->display->duration_ms);
    g_key_file_set_integer (keyfile, group, "year",
                            track->display->release_year);
    g_key_file_set_integer (keyfile, group, "track", track->track_number);
    g_key_file_set_integer (keyfile, group, "disc", track->disc_number);
    g_key_file_set_integer (keyfile, group, "rate", track->sample_rate);
    g_key_file_set_integer (keyfile, group, "channels", track->channels);
    g_key_file_set_integer (keyfile, group, "bits", track->source_bits);
  }
  gsize length = 0;
  g_autofree gchar *payload = g_key_file_to_data (keyfile, &length, NULL);
  if (!payload || length > LOCAL_INDEX_MAX_BYTES - 80) return;
  g_autofree gchar *digest = g_compute_checksum_for_data (
    G_CHECKSUM_SHA256, (const guchar *) payload, length);
  g_autofree gchar *content = g_strconcat (LOCAL_INDEX_MAGIC, digest, "\n",
                                           payload, NULL);
  g_autofree gchar *path = index_path ();
  g_autofree gchar *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0) return;
  g_file_set_contents_full (path, content, -1,
    G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, NULL);
}

static void
scan_directory (SpotifyGtkLocalSnapshot *s, GFile *directory, guint depth,
                GHashTable *cached, GHashTable *art_dirs,
                GCancellable *cancel)
{
  if (depth > LOCAL_MAX_DEPTH || s->tracks->len >= LOCAL_MAX_FILES ||
      g_cancellable_is_cancelled (cancel)) return;
  g_autofree gchar *dir_path = g_file_get_path (directory);
  if (!dir_path) return;
  g_ptr_array_add (s->directories, g_strdup (dir_path));
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileEnumerator) e = g_file_enumerate_children (
    directory, G_FILE_ATTRIBUTE_STANDARD_NAME ","
               G_FILE_ATTRIBUTE_STANDARD_TYPE ","
               G_FILE_ATTRIBUTE_STANDARD_SIZE ","
               G_FILE_ATTRIBUTE_TIME_MODIFIED ","
               G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &error);
  if (!e) return;
  while (!g_cancellable_is_cancelled (cancel) &&
         s->tracks->len < LOCAL_MAX_FILES) {
    g_autoptr(GFileInfo) info = g_file_enumerator_next_file (e, cancel, NULL);
    if (!info) break;
    const gchar *name = g_file_info_get_name (info);
    if (!name || name[0] == '.') continue;
    GFileType type = g_file_info_get_file_type (info);
    if (type != G_FILE_TYPE_DIRECTORY &&
        (type != G_FILE_TYPE_REGULAR || !supported_name (name))) continue;
    g_autoptr(GFile) child = g_file_get_child (directory, name);
    if (type == G_FILE_TYPE_DIRECTORY) {
      scan_directory (s, child, depth + 1, cached, art_dirs, cancel);
      continue;
    }
#if HAVE_LOCAL_AV
    g_autofree gchar *path = g_file_get_path (child);
    guint64 size = g_file_info_get_size (info);
    if (!path || !size || size > G_GUINT64_CONSTANT (1099511627776)) continue;
    gint64 mtime = (gint64) g_file_info_get_attribute_uint64 (
      info, G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC +
      g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
    SpotifyGtkLocalTrack *track = g_hash_table_lookup (cached, path);
    if (track && track->file_size == size && track->mtime_us == mtime &&
        cached_art_matches (track) &&
        (!art_dirs || !g_hash_table_contains (art_dirs, dir_path))) {
      g_hash_table_steal (cached, path);
      if (track->art_path && track->display->cover_id)
        g_hash_table_replace (s->art_paths,
          g_strdup (track->display->cover_id), g_strdup (track->art_path));
    } else {
      track = probe_track (s, path, size, mtime);
    }
    if (track) {
      g_ptr_array_add (s->tracks, track);
      g_hash_table_insert (s->by_track, track->display->uri, track);
    }
#endif
  }
  g_file_enumerator_close (e, cancel, NULL);
}

static gint
track_order (gconstpointer a, gconstpointer b)
{
  const SpotifyGtkLocalTrack *x = *(SpotifyGtkLocalTrack *const *) a;
  const SpotifyGtkLocalTrack *y = *(SpotifyGtkLocalTrack *const *) b;
  if (x->disc_number != y->disc_number)
    return x->disc_number < y->disc_number ? -1 : 1;
  if (x->track_number != y->track_number)
    return x->track_number < y->track_number ? -1 : 1;
  return g_strcmp0 (x->display->name, y->display->name);
}

static gint
album_order (gconstpointer a, gconstpointer b)
{
  const SpotifyGtkLocalAlbum *x = *(SpotifyGtkLocalAlbum *const *) a;
  const SpotifyGtkLocalAlbum *y = *(SpotifyGtkLocalAlbum *const *) b;
  if (x->added_at != y->added_at) return x->added_at > y->added_at ? -1 : 1;
  return g_strcmp0 (x->title, y->title);
}

static gchar *
album_directory (const gchar *path)
{
  gchar *directory = g_path_get_dirname (path);
  g_autofree gchar *base = g_path_get_basename (directory);
  const gchar *number = NULL;
  if (g_ascii_strncasecmp (base, "disc", 4) == 0 ||
      g_ascii_strncasecmp (base, "disk", 4) == 0)
    number = base + 4;
  else if (g_ascii_strncasecmp (base, "cd", 2) == 0)
    number = base + 2;
  if (number) {
    while (*number == ' ' || *number == '-' || *number == '_') number++;
    gboolean numbered = g_ascii_isdigit (*number);
    while (g_ascii_isdigit (*number)) number++;
    if (numbered && *number == '\0') {
      gchar *parent = g_path_get_dirname (directory);
      g_free (directory);
      return parent;
    }
  }
  return directory;
}

static void
group_albums (SpotifyGtkLocalSnapshot *s)
{
  for (guint i = 0; i < s->tracks->len; i++) {
    SpotifyGtkLocalTrack *t = g_ptr_array_index (s->tracks, i);
    g_autofree gchar *album_name = normal_key (t->display->album);
    g_autofree gchar *artist_name = normal_key (t->album_artist);
    g_autofree gchar *directory = album_directory (t->path);
    g_autofree gchar *key = g_strdup_printf ("%s\n%s\n%d\n%s", album_name,
                                            artist_name,
                                            t->display->release_year, directory);
    g_autofree gchar *uri = hashed_uri ("local:album:", key);
    SpotifyGtkLocalAlbum *album = g_hash_table_lookup (s->by_album, uri);
    if (!album) {
      album = g_new0 (SpotifyGtkLocalAlbum, 1);
      album->uri = g_strdup (uri);
      album->title = g_strdup (t->display->album);
      album->artist = g_strdup (t->album_artist);
      album->year = t->display->release_year;
      album->tracks = g_ptr_array_new ();
      g_ptr_array_add (s->albums, album);
      g_hash_table_insert (s->by_album, album->uri, album);
    }
    g_free (t->display->album_uri);
    t->display->album_uri = g_strdup (album->uri);
    album->added_at = MAX (album->added_at, t->mtime_us);
    g_ptr_array_add (album->tracks, t);
  }
  for (guint i = 0; i < s->albums->len; i++) {
    SpotifyGtkLocalAlbum *album = g_ptr_array_index (s->albums, i);
    g_ptr_array_sort (album->tracks, track_order);
    for (guint j = 0; j < album->tracks->len; j++) {
      const SpotifyGtkLocalTrack *track = g_ptr_array_index (album->tracks, j);
      if (track->display->cover_id) {
        album->cover_id = g_strdup (track->display->cover_id);
        break;
      }
    }
  }
  g_ptr_array_sort (s->albums, album_order);
}

typedef struct {
  GPtrArray *roots; /* gchar* copies */
  GPtrArray *dirty_dirs; /* NULL means full reconciliation */
  GPtrArray *known_dirs; /* previous monitored directories */
  GHashTable *art_dirs; /* copied path set */
  guint generation;
} ScanRequest;

static void
scan_request_free (gpointer data)
{
  ScanRequest *request = data;
  g_ptr_array_unref (request->roots);
  g_clear_pointer (&request->dirty_dirs, g_ptr_array_unref);
  g_clear_pointer (&request->known_dirs, g_ptr_array_unref);
  g_clear_pointer (&request->art_dirs, g_hash_table_unref);
  g_free (request);
}

static void
scan_worker (GTask *task, gpointer source, gpointer task_data,
             GCancellable *cancel)
{
  ScanRequest *request = task_data;
  SpotifyGtkLocalSnapshot *s = snapshot_new ();
#if !HAVE_LOCAL_AV
  g_task_return_pointer (task, s,
                         (GDestroyNotify) spotifygtk_local_snapshot_unref);
  (void) source; (void) cancel;
  return;
#endif
  g_autoptr(GHashTable) cached = request->roots->len > 0
    ? load_index ()
    : g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                             local_track_free);
  if (request->dirty_dirs) {
    /* Retain indexed entries outside changed subtrees without walking or
     * probing their directories. The new snapshot owns each stolen track. */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, cached);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
      SpotifyGtkLocalTrack *track = value;
      if (path_in_any_directory (track->path, request->dirty_dirs)) continue;
      g_hash_table_iter_steal (&iter);
      g_ptr_array_add (s->tracks, track);
      g_hash_table_insert (s->by_track, track->display->uri, track);
      if (track->art_path && track->display->cover_id)
        g_hash_table_replace (s->art_paths, g_strdup (track->display->cover_id),
                              g_strdup (track->art_path));
    }
    for (guint i = 0; i < request->known_dirs->len; i++) {
      const gchar *known = g_ptr_array_index (request->known_dirs, i);
      if (!path_in_any_directory (known, request->dirty_dirs))
        g_ptr_array_add (s->directories, g_strdup (known));
    }
    for (guint i = 0; i < request->dirty_dirs->len &&
                      !g_cancellable_is_cancelled (cancel); i++) {
      const gchar *path = g_ptr_array_index (request->dirty_dirs, i);
      gboolean covered = FALSE;
      for (guint j = 0; j < request->dirty_dirs->len; j++) {
        const gchar *other = g_ptr_array_index (request->dirty_dirs, j);
        if (j != i && path_in_directory (path, other) &&
            g_strcmp0 (path, other) != 0) { covered = TRUE; break; }
      }
      if (covered) continue;
      g_autoptr(GFile) dir = g_file_new_for_path (path);
      scan_directory (s, dir, 0, cached, request->art_dirs, cancel);
    }
  } else {
    for (guint i = 0; i < request->roots->len &&
                      !g_cancellable_is_cancelled (cancel); i++) {
      const gchar *path = g_ptr_array_index (request->roots, i);
      gboolean nested = FALSE;
      for (guint j = 0; j < request->roots->len; j++) {
        const gchar *other = g_ptr_array_index (request->roots, j);
        if (i != j && path_in_directory (path, other) &&
            g_strcmp0 (path, other) != 0) { nested = TRUE; break; }
      }
      if (nested) continue;
      g_autoptr(GFile) root = g_file_new_for_path (path);
      scan_directory (s, root, 0, cached, NULL, cancel);
    }
  }
  if (g_cancellable_is_cancelled (cancel)) {
    spotifygtk_local_snapshot_unref (s);
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "Local scan cancelled");
    return;
  }
  group_albums (s);
  if (!g_cancellable_is_cancelled (cancel) && request->roots->len > 0)
    save_index (s);
  g_task_return_pointer (task, s,
                         (GDestroyNotify) spotifygtk_local_snapshot_unref);
  (void) source;
}

static void start_scan (SpotifyGtkLocalCatalog *self, gboolean incremental);

static gboolean
schedule_rescan (gpointer data)
{
  SpotifyGtkLocalCatalog *self = data;
  self->rescan_id = 0;
  start_scan (self, TRUE);
  return G_SOURCE_REMOVE;
}

static gboolean
sidecar_name (GFile *file)
{
  g_autofree gchar *base = g_file_get_basename (file);
  return base && (g_ascii_strcasecmp (base, "cover.jpg") == 0 ||
                  g_ascii_strcasecmp (base, "cover.jpeg") == 0 ||
                  g_ascii_strcasecmp (base, "cover.png") == 0 ||
                  g_ascii_strcasecmp (base, "folder.jpg") == 0 ||
                  g_ascii_strcasecmp (base, "folder.png") == 0);
}

static void
mark_dirty_parent (SpotifyGtkLocalCatalog *self, GFile *file,
                   gboolean art_changed)
{
  if (!file) return;
  g_autofree gchar *file_path = g_file_get_path (file);
  if (!file_path) return;
  g_autoptr(GFile) parent = g_file_get_parent (file);
  g_autofree gchar *path = parent ? g_file_get_path (parent) : NULL;
  if (!path) return;
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  const GPtrArray *roots = spotifygtk_settings_get_local_files_enabled (settings)
    ? spotifygtk_settings_get_local_directories (settings) : NULL;
  for (guint i = 0; roots && i < roots->len; i++) {
    const gchar *root = g_ptr_array_index ((GPtrArray *) roots, i);
    if (!path_in_directory (file_path, root) &&
        g_strcmp0 (file_path, root) != 0)
      continue;
    const gchar *target = path_in_directory (path, root) ? path :
      path_in_directory (root, path) ? root : NULL;
    if (!target) continue;
    g_hash_table_add (self->dirty_dirs, g_strdup (target));
    if (art_changed)
      g_hash_table_add (self->art_dirs, g_strdup (target));
  }
}

static void
on_directory_event (GFileMonitor *monitor, GFile *file, GFile *other,
                    GFileMonitorEvent event, gpointer data)
{
  SpotifyGtkLocalCatalog *self = data;
  if (event == G_FILE_MONITOR_EVENT_PRE_UNMOUNT) return;
  mark_dirty_parent (self, file, file && sidecar_name (file));
  mark_dirty_parent (self, other, other && sidecar_name (other));
  if (g_hash_table_size (self->dirty_dirs) == 0) return;
  if (self->rescan_id) g_source_remove (self->rescan_id);
  self->rescan_id = g_timeout_add (750, schedule_rescan, self);
  (void) monitor; (void) file; (void) other;
}

static void
replace_monitors (SpotifyGtkLocalCatalog *self,
                  SpotifyGtkLocalSnapshot *s)
{
  g_autoptr(GHashTable) wanted = g_hash_table_new_full (
    g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < s->directories->len; i++)
    g_hash_table_add (wanted, g_strdup (g_ptr_array_index (s->directories, i)));
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  const GPtrArray *roots = spotifygtk_settings_get_local_files_enabled (settings)
    ? spotifygtk_settings_get_local_directories (settings) : NULL;
  for (guint i = 0; roots && i < roots->len; i++) {
    const gchar *root = g_ptr_array_index ((GPtrArray *) roots, i);
    g_autofree gchar *parent = g_path_get_dirname (root);
    g_hash_table_add (wanted, g_strdup (parent));
  }
  for (guint i = self->monitors->len; i > 0; i--) {
    GFileMonitor *monitor = g_ptr_array_index (self->monitors, i - 1);
    const gchar *path = g_object_get_data (G_OBJECT (monitor), "local-path");
    if (g_hash_table_contains (wanted, path)) {
      g_hash_table_remove (wanted, path);
      continue;
    }
    g_signal_handlers_disconnect_by_data (monitor, self);
    g_file_monitor_cancel (monitor);
    g_ptr_array_remove_index (self->monitors, i - 1);
  }
  GHashTableIter wanted_iter;
  gpointer wanted_key;
  g_hash_table_iter_init (&wanted_iter, wanted);
  while (g_hash_table_iter_next (&wanted_iter, &wanted_key, NULL)) {
    const gchar *path = wanted_key;
    g_autoptr(GFile) dir = g_file_new_for_path (
      path);
    g_autoptr(GError) error = NULL;
    GFileMonitor *monitor = g_file_monitor_directory (
      dir, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
    if (!monitor) continue;
    g_object_set_data_full (G_OBJECT (monitor), "local-path", g_strdup (path), g_free);
    g_signal_connect (monitor, "changed", G_CALLBACK (on_directory_event), self);
    g_ptr_array_add (self->monitors, monitor);
  }
}

static void
on_scan_done (GObject *source, GAsyncResult *result, gpointer data)
{
  SpotifyGtkLocalCatalog *self = SPOTIFYGTK_LOCAL_CATALOG (source);
  guint generation = GPOINTER_TO_UINT (data);
  g_autoptr(GError) error = NULL;
  SpotifyGtkLocalSnapshot *next = g_task_propagate_pointer (G_TASK (result),
                                                            &error);
  if (generation != self->generation) {
    spotifygtk_local_snapshot_unref (next);
    return;
  }
  self->scan_active = FALSE;
  g_clear_object (&self->scan_cancel);
  if (!next) {
    if (g_hash_table_size (self->dirty_dirs) > 0 && !self->rescan_id)
      self->rescan_id = g_timeout_add (750, schedule_rescan, self);
    return;
  }
  replace_monitors (self, next);
  g_mutex_lock (&snapshot_lock);
  SpotifyGtkLocalSnapshot *old = self->snapshot;
  self->snapshot = next;
  g_mutex_unlock (&snapshot_lock);
  spotifygtk_local_snapshot_unref (old);
  g_signal_emit (self, changed_signal, 0);
  if (g_hash_table_size (self->dirty_dirs) > 0 && !self->rescan_id)
    self->rescan_id = g_timeout_add (750, schedule_rescan, self);
}

static void
start_scan (SpotifyGtkLocalCatalog *self, gboolean incremental)
{
  g_return_if_fail (SPOTIFYGTK_IS_LOCAL_CATALOG (self));
  if (incremental && g_hash_table_size (self->dirty_dirs) == 0)
    return;
  if (incremental && self->scan_active)
    return; /* keep dirty paths for the next batch */
  if (self->scan_cancel) g_cancellable_cancel (self->scan_cancel);
  g_clear_object (&self->scan_cancel);
  self->scan_cancel = g_cancellable_new ();
  self->scan_active = TRUE;
  self->generation++;
  ScanRequest *request = g_new0 (ScanRequest, 1);
  request->generation = self->generation;
  request->roots = g_ptr_array_new_with_free_func (g_free);
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  if (spotifygtk_settings_get_local_files_enabled (settings)) {
    const GPtrArray *configured = spotifygtk_settings_get_local_directories (settings);
    for (guint i = 0; configured && i < configured->len; i++)
      g_ptr_array_add (request->roots,
                       g_strdup (g_ptr_array_index ((GPtrArray *) configured, i)));
  }
  if (incremental) {
    request->dirty_dirs = g_ptr_array_new_with_free_func (g_free);
    request->known_dirs = g_ptr_array_new_with_free_func (g_free);
    request->art_dirs = g_hash_table_new_full (
      g_str_hash, g_str_equal, g_free, NULL);
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init (&iter, self->dirty_dirs);
    while (g_hash_table_iter_next (&iter, &key, NULL))
      g_ptr_array_add (request->dirty_dirs, g_strdup (key));
    g_hash_table_iter_init (&iter, self->art_dirs);
    while (g_hash_table_iter_next (&iter, &key, NULL))
      g_hash_table_add (request->art_dirs, g_strdup (key));
    g_mutex_lock (&snapshot_lock);
    for (guint i = 0; self->snapshot && i < self->snapshot->directories->len; i++)
      g_ptr_array_add (request->known_dirs,
        g_strdup (g_ptr_array_index (self->snapshot->directories, i)));
    g_mutex_unlock (&snapshot_lock);
  }
  g_hash_table_remove_all (self->dirty_dirs);
  g_hash_table_remove_all (self->art_dirs);
  GTask *task = g_task_new (self, self->scan_cancel, on_scan_done,
                            GUINT_TO_POINTER (self->generation));
  g_task_set_return_on_cancel (task, FALSE);
  g_task_set_task_data (task, request, scan_request_free);
  g_task_run_in_thread (task, scan_worker);
  g_object_unref (task);
}

void
spotifygtk_local_catalog_refresh (SpotifyGtkLocalCatalog *self)
{
  start_scan (self, FALSE);
}

static void
catalog_dispose (GObject *object)
{
  SpotifyGtkLocalCatalog *self = SPOTIFYGTK_LOCAL_CATALOG (object);
  g_clear_handle_id (&self->rescan_id, g_source_remove);
  g_clear_pointer (&self->dirty_dirs, g_hash_table_unref);
  g_clear_pointer (&self->art_dirs, g_hash_table_unref);
  if (self->scan_cancel) g_cancellable_cancel (self->scan_cancel);
  g_clear_object (&self->scan_cancel);
  if (self->monitors) {
    for (guint i = 0; i < self->monitors->len; i++) {
      GFileMonitor *monitor = g_ptr_array_index (self->monitors, i);
      g_signal_handlers_disconnect_by_data (monitor, self);
      g_file_monitor_cancel (monitor);
    }
    g_ptr_array_unref (self->monitors);
    self->monitors = NULL;
  }
  g_mutex_lock (&snapshot_lock);
  if (global_catalog == self) global_catalog = NULL;
  SpotifyGtkLocalSnapshot *s = self->snapshot;
  self->snapshot = NULL;
  g_mutex_unlock (&snapshot_lock);
  spotifygtk_local_snapshot_unref (s);
  G_OBJECT_CLASS (spotifygtk_local_catalog_parent_class)->dispose (object);
}

static void
spotifygtk_local_catalog_class_init (SpotifyGtkLocalCatalogClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = catalog_dispose;
  changed_signal = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
spotifygtk_local_catalog_init (SpotifyGtkLocalCatalog *self)
{
  self->snapshot = snapshot_new ();
  self->monitors = g_ptr_array_new_with_free_func (g_object_unref);
  self->dirty_dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->art_dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_signal_connect_object (spotifygtk_settings_get_default (),
                           "local-files-changed",
                           G_CALLBACK (spotifygtk_local_catalog_refresh),
                           self, G_CONNECT_SWAPPED);
}

SpotifyGtkLocalCatalog *
spotifygtk_local_catalog_get_default (void)
{
  if (!global_catalog) {
    global_catalog = g_object_new (SPOTIFYGTK_TYPE_LOCAL_CATALOG, NULL);
    spotifygtk_local_catalog_refresh (global_catalog);
  }
  return global_catalog;
}

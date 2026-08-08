/*
 * track_meta.c — Display metadata from a metadata.proto `Track` message.
 */

#include "track_meta.h"
#include "protobuf_min.h"

#include <string.h>

/* metadata.proto, message Track */
#define TRACK_FIELD_NAME      2
#define TRACK_FIELD_ALBUM     3
#define TRACK_FIELD_ARTIST    4
#define TRACK_FIELD_DURATION  7
#define TRACK_FIELD_EXPLICIT  9

/* Album.name and Artist.name are both field 2 of their own message. */
#define NAMED_MESSAGE_FIELD_NAME 2

/* Album.gid and Artist.gid are both field 1 (bytes) of their own message —
 * the raw 16-byte SpotifyId behind spotify:album:<id> / spotify:artist:<id>. */
#define NAMED_MESSAGE_FIELD_GID  1

/* metadata.proto: Album.cover_group -> ImageGroup.image -> Image */
#define ALBUM_FIELD_DATE           6
#define ALBUM_FIELD_COVER_GROUP   17
#define DATE_FIELD_YEAR            1
#define IMAGE_GROUP_FIELD_IMAGE    1
#define IMAGE_FIELD_FILE_ID        1
#define IMAGE_FIELD_WIDTH          3

static gchar *
bytes_to_hex (const guint8 *data, gsize len)
{
  GString *hex = g_string_sized_new (len * 2);
  for (gsize i = 0; i < len; i++)
    g_string_append_printf (hex, "%02x", data[i]);
  return g_string_free (hex, FALSE);
}

/*
 * Pick the largest cover from an Album's ImageGroup.
 *
 * Spotify advertises several sizes; the widest is chosen because the UI
 * scales down (the Now Playing cover tracks the panel width) and scaling a
 * small source up looks worse than scaling a large one down.
 *
 * Image.width is sint32, so it is zigzag-encoded — the same trap that made
 * every track duration come back doubled. Only relative order matters here,
 * and zigzag preserves order for non-negative values, so the comparison
 * would still pick the right image undecoded; it is decoded anyway so the
 * value is not quietly wrong if anyone later reads it.
 */
/*
 * Release year from Album.date.
 *
 * Date.year is sint32, so zigzag-encoded -- the same trap that made every
 * track duration come back doubled. Unlike the cover comparison, the decoded
 * value is used directly here, so getting this wrong would show a wrong year
 * rather than merely a wrong ordering.
 *
 * Field numbers are taken from metadata.proto rather than observed, so the
 * result is range-checked: a wrong field number parses into a plausible-looking
 * integer rather than failing, and a silently wrong year on every album is
 * worse than no year at all. Anything outside living memory of recorded music
 * is treated as absent.
 */
static gint
album_release_year (const guint8 *album_data, gsize album_len)
{
  const guint8 *date_data = NULL;
  gsize         date_len  = 0;

  if (!pb_find_bytes_field (album_data, album_len, ALBUM_FIELD_DATE,
                            &date_data, &date_len))
    return 0;

  guint64 raw = 0;
  if (!pb_find_varint_field (date_data, date_len, DATE_FIELD_YEAR, &raw))
    return 0;

  gint64 year = (gint64) ((raw >> 1) ^ (~(raw & 1) + 1));
  if (year < 1860 || year > 2200) {
    g_debug ("track_meta: implausible release year %" G_GINT64_FORMAT
             " -- treating as absent", year);
    return 0;
  }
  return (gint) year;
}

/*
 * Pick a cover from the album's image group.
 *
 * Every album offers three widths -- 64, 300 and 640, confirmed live -- and
 * only the widest was ever kept, so a 96px row thumbnail downloaded the 640px
 * original: 112.5 KB of payload, measured, to produce 36 KB of pixels, once
 * per row of a five-thousand-track library.
 *
 * `min_width` is the narrowest acceptable variant: the smallest one at least
 * that wide wins, falling back to the widest available when nothing qualifies.
 * Not simply "the smallest", which would hand a row the 64px image for a 96px
 * box and upscale it.
 *
 * 0 means "the widest", not "anything goes" -- with no floor every variant
 * qualifies and the narrowest rule would hand the panel the 64px thumbnail.
 */
static gchar *
dup_cover_id (const guint8 *album_data, gsize album_len, gint64 min_width)
{
  const guint8 *group_data = NULL;
  gsize         group_len  = 0;

  if (!pb_find_bytes_field (album_data, album_len, ALBUM_FIELD_COVER_GROUP,
                            &group_data, &group_len))
    return NULL;

  gsize         pos = 0;
  guint32       field_num;
  PbWireType    wire_type;
  const guint8 *field_data;
  gsize         field_len;
  guint64       field_varint;

  gchar  *best_id     = NULL;   /* narrowest that qualifies */
  gint64  best_width  = -1;
  gchar  *widest_id   = NULL;   /* fallback when nothing does */
  gint64  widest      = -1;

  while (pb_read_field (group_data, group_len, &pos, &field_num, &wire_type,
                        &field_data, &field_len, &field_varint)) {
    if (field_num != IMAGE_GROUP_FIELD_IMAGE || wire_type != PB_WIRE_LENGTH_DELIMITED)
      continue;

    const guint8 *id_data = NULL;
    gsize         id_len  = 0;
    if (!pb_find_bytes_field (field_data, field_len, IMAGE_FIELD_FILE_ID,
                              &id_data, &id_len))
      continue;

    guint64 raw_width = 0;
    gint64  width = 0;
    if (pb_find_varint_field (field_data, field_len, IMAGE_FIELD_WIDTH, &raw_width))
      width = (gint64) ((raw_width >> 1) ^ (~(raw_width & 1) + 1));

    g_autofree gchar *id = bytes_to_hex (id_data, id_len);

    if (width > widest || widest_id == NULL) {
      g_free (widest_id);
      widest_id = g_strdup (id);
      widest    = width;
    }

    if (width >= min_width && (best_id == NULL || width < best_width)) {
      g_free (best_id);
      best_id    = g_strdup (id);
      best_width = width;
    }
  }

  if (min_width <= 0) {
    g_free (best_id);
    return widest_id;
  }

  if (best_id) {
    g_free (widest_id);
    return best_id;
  }
  return widest_id;
}

static gchar *
dup_name_from_submessage (const guint8 *data, gsize len)
{
  const guint8 *name_data = NULL;
  gsize         name_len  = 0;

  if (!pb_find_bytes_field (data, len, NAMED_MESSAGE_FIELD_NAME, &name_data, &name_len))
    return NULL;

  return g_strndup ((const gchar *) name_data, name_len);
}

/*
 * Encode a 16-byte SpotifyId gid as its canonical 22-character base62 id.
 *
 * The gid is a big-endian 128-bit integer; the id is that integer written in
 * base62 (0-9 a-z A-Z, in that order), left-padded with '0' to a fixed 22
 * digits. This mirrors librespot's SpotifyId::to_base62. Anything but a
 * 16-byte gid is rejected — a short or long buffer is not a SpotifyId and
 * must not be silently truncated into a plausible-looking wrong id.
 */
gchar *
spotifygtk_gid_to_base62 (const guint8 *gid, gsize len)
{
  static const char ALPHABET[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

  if (!gid || len != 16)
    return NULL;

  /* Repeated divide-by-62 of the 128-bit value held as a 16-byte big-endian
   * buffer. Each pass produces one base62 digit (least significant first);
   * exactly 22 digits span the range, so the loop is fixed-length. */
  guint8 n[16];
  memcpy (n, gid, 16);

  gchar out[23];
  out[22] = '\0';

  for (int digit = 21; digit >= 0; digit--) {
    guint remainder = 0;
    for (int i = 0; i < 16; i++) {
      guint acc = (remainder << 8) | n[i];
      n[i]      = (guint8) (acc / 62);
      remainder = acc % 62;
    }
    out[digit] = ALPHABET[remainder];
  }

  return g_strdup (out);
}

/* Build "spotify:<kind>:<id>" from a submessage's gid field, or NULL if the
 * submessage carries no (valid) gid. */
static gchar *
dup_uri_from_submessage (const guint8 *data, gsize len, const gchar *kind)
{
  const guint8 *gid_data = NULL;
  gsize         gid_len  = 0;

  if (!pb_find_bytes_field (data, len, NAMED_MESSAGE_FIELD_GID, &gid_data, &gid_len))
    return NULL;

  g_autofree gchar *id = spotifygtk_gid_to_base62 (gid_data, gid_len);
  if (!id)
    return NULL;

  return g_strconcat ("spotify:", kind, ":", id, NULL);
}

gboolean
spotifygtk_track_meta_parse (const guint8     *track_data,
                             gsize             track_len,
                             SpotifyTrackMeta *out)
{
  g_return_val_if_fail (out != NULL, FALSE);

  memset (out, 0, sizeof *out);

  if (!track_data || track_len == 0)
    return FALSE;

  GPtrArray *artists = g_ptr_array_new_with_free_func (g_free);

  gsize        pos = 0;
  guint32      field_num;
  PbWireType   wire_type;
  const guint8 *field_data;
  gsize         field_len;
  guint64       field_varint;
  gboolean      read_any = FALSE;

  while (pb_read_field (track_data, track_len, &pos, &field_num, &wire_type,
                        &field_data, &field_len, &field_varint)) {
    read_any = TRUE;

    switch (field_num) {
    case TRACK_FIELD_NAME:
      if (wire_type == PB_WIRE_LENGTH_DELIMITED && !out->name)
        out->name = g_strndup ((const gchar *) field_data, field_len);
      break;

    case TRACK_FIELD_ALBUM:
      if (wire_type == PB_WIRE_LENGTH_DELIMITED && !out->album_name)
        out->album_name = dup_name_from_submessage (field_data, field_len);
      if (wire_type == PB_WIRE_LENGTH_DELIMITED && !out->cover_id)
        out->cover_id = dup_cover_id (field_data, field_len, 0);
      if (wire_type == PB_WIRE_LENGTH_DELIMITED && !out->cover_id_small)
        /* Rows draw at 96px, cards at 176; 128 lands on the 300px variant,
         * which covers both without reaching for the 640. */
        out->cover_id_small = dup_cover_id (field_data, field_len, 128);
      if (wire_type == PB_WIRE_LENGTH_DELIMITED && !out->album_uri)
        out->album_uri = dup_uri_from_submessage (field_data, field_len, "album");
      if (wire_type == PB_WIRE_LENGTH_DELIMITED && out->release_year == 0)
        out->release_year = album_release_year (field_data, field_len);
      break;

    case TRACK_FIELD_ARTIST:
      /* Repeated: every occurrence is one artist, in order. */
      if (wire_type == PB_WIRE_LENGTH_DELIMITED) {
        gchar *artist = dup_name_from_submessage (field_data, field_len);
        if (artist)
          g_ptr_array_add (artists, artist);
        /* "Go to Artist" navigates to the primary (first) artist only. */
        if (!out->artist_uri)
          out->artist_uri = dup_uri_from_submessage (field_data, field_len, "artist");
      }
      break;

    case TRACK_FIELD_DURATION:
      /* sint32, so genuinely zigzag-encoded -- confirmed against live data,
       * not assumed. Reading it as a plain varint returns exactly double:
       * Rick Astley's "Never Gonna Give You Up" came back as 7:07 instead
       * of 3:33, a-ha's "Take on Me" as 7:30 instead of 3:45. For positive
       * n zigzag stores 2n, which is precisely that 2x.
       *
       * An earlier revision of this file claimed the opposite in a comment
       * ("Spotify encodes a plain varint") -- it was wrong, and only a
       * live response showed it. */
      if (wire_type == PB_WIRE_VARINT)
        out->duration_ms = (gint64) ((field_varint >> 1) ^ (~(field_varint & 1) + 1));
      break;

    case TRACK_FIELD_EXPLICIT:
      if (wire_type == PB_WIRE_VARINT)
        out->is_explicit = (field_varint != 0);
      break;

    default:
      break;
    }
  }

  if (!read_any) {
    g_ptr_array_free (artists, TRUE);
    return FALSE;
  }

  if (artists->len > 0) {
    g_ptr_array_add (artists, NULL);   /* NULL-terminate for g_strjoinv */
    out->artist_names = g_strjoinv (", ", (gchar **) artists->pdata);
  }

  g_ptr_array_free (artists, TRUE);
  return TRUE;
}

void
spotifygtk_track_meta_clear (SpotifyTrackMeta *meta)
{
  if (!meta)
    return;

  g_clear_pointer (&meta->name, g_free);
  g_clear_pointer (&meta->album_name, g_free);
  g_clear_pointer (&meta->artist_names, g_free);
  g_clear_pointer (&meta->cover_id, g_free);
  g_clear_pointer (&meta->album_uri, g_free);
  g_clear_pointer (&meta->artist_uri, g_free);
  meta->duration_ms = 0;
  meta->is_explicit = FALSE;
}

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

static gchar *
dup_name_from_submessage (const guint8 *data, gsize len)
{
  const guint8 *name_data = NULL;
  gsize         name_len  = 0;

  if (!pb_find_bytes_field (data, len, NAMED_MESSAGE_FIELD_NAME, &name_data, &name_len))
    return NULL;

  return g_strndup ((const gchar *) name_data, name_len);
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
      break;

    case TRACK_FIELD_ARTIST:
      /* Repeated: every occurrence is one artist, in order. */
      if (wire_type == PB_WIRE_LENGTH_DELIMITED) {
        gchar *artist = dup_name_from_submessage (field_data, field_len);
        if (artist)
          g_ptr_array_add (artists, artist);
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
  meta->duration_ms = 0;
  meta->is_explicit = FALSE;
}

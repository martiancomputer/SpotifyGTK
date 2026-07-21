/*
 * test_track_meta.c — Track display-metadata extraction.
 *
 * Builds a metadata.proto `Track` message with the project's own encoder
 * and reads it back, so the field numbers in track_meta.c are checked
 * against the schema rather than against a comment claiming they match.
 *
 * This is deliberately not a round-trip of one function against itself:
 * the expected field numbers below are transcribed from librespot's
 * protocol/proto/metadata.proto and written as literals here, so if
 * track_meta.c ever drifts to a different field, this fails.
 */

#include <glib.h>
#include <string.h>

#include "spotify/protobuf_min.h"
#include "spotify/track_meta.h"

/* metadata.proto declares Track.duration as sint32, so it goes on the wire
 * zigzag-encoded. Encoding it here the way the server does is the point of
 * these assertions: reading it back as a plain varint yields exactly 2x,
 * which is how the bug originally showed up against live data. */
static guint64
zigzag_encode (gint32 value)
{
  return (guint64) (((guint32) value << 1) ^ (guint32) (value >> 31));
}

/* Encode a `name`-only submessage, which is how both Album and Artist
 * carry their display name (field 2 of each). */
static GByteArray *
build_named_submessage (const gchar *name)
{
  GByteArray *buf = g_byte_array_new ();
  pb_write_bytes_field (buf, 2, (const guint8 *) name, strlen (name));
  return buf;
}

static void
test_full_track (void)
{
  GByteArray *track = g_byte_array_new ();

  /* Track.name = 2 */
  const gchar *name = "Never Gonna Give You Up";
  pb_write_bytes_field (track, 2, (const guint8 *) name, strlen (name));

  /* Track.album = 3 (Album.name = 2) */
  GByteArray *album = build_named_submessage ("Whenever You Need Somebody");
  pb_write_message_field (track, 3, album->data, album->len);
  g_byte_array_free (album, TRUE);

  /* Track.artist = 4, repeated — two of them, order preserved */
  GByteArray *a1 = build_named_submessage ("Rick Astley");
  pb_write_message_field (track, 4, a1->data, a1->len);
  g_byte_array_free (a1, TRUE);

  GByteArray *a2 = build_named_submessage ("Some Collaborator");
  pb_write_message_field (track, 4, a2->data, a2->len);
  g_byte_array_free (a2, TRUE);

  /* Track.duration = 7, milliseconds. 213000 ms = 3:33, the real length of
   * this track -- the value the live probe returned as 7:07 before the
   * zigzag fix. */
  pb_write_varint_field (track, 7, zigzag_encode (213000));

  /* Track.explicit = 9 */
  pb_write_varint_field (track, 9, 1);

  SpotifyTrackMeta meta;
  g_assert_true (spotifygtk_track_meta_parse (track->data, track->len, &meta));

  g_assert_cmpstr (meta.name, ==, "Never Gonna Give You Up");
  g_assert_cmpstr (meta.album_name, ==, "Whenever You Need Somebody");
  g_assert_cmpstr (meta.artist_names, ==, "Rick Astley, Some Collaborator");
  g_assert_cmpint (meta.duration_ms, ==, 213000);
  g_assert_true (meta.is_explicit);

  spotifygtk_track_meta_clear (&meta);
  g_byte_array_free (track, TRUE);
}

/* A Track carrying only the AudioFile entries playback needs must not be
 * mistaken for a parse failure — it is a valid message, just without any
 * display fields. The UI decides what to render for a missing title. */
static void
test_track_without_display_fields (void)
{
  GByteArray *track = g_byte_array_new ();

  /* Track.file = 12, an AudioFile with only file_id (field 1). */
  GByteArray *file = g_byte_array_new ();
  const guint8 file_id[20] = { 0xAA, 0xBB, 0xCC };
  pb_write_bytes_field (file, 1, file_id, sizeof file_id);
  pb_write_message_field (track, 12, file->data, file->len);
  g_byte_array_free (file, TRUE);

  SpotifyTrackMeta meta;
  g_assert_true (spotifygtk_track_meta_parse (track->data, track->len, &meta));

  g_assert_null (meta.name);
  g_assert_null (meta.album_name);
  g_assert_null (meta.artist_names);
  g_assert_cmpint (meta.duration_ms, ==, 0);
  g_assert_false (meta.is_explicit);

  spotifygtk_track_meta_clear (&meta);
  g_byte_array_free (track, TRUE);
}

/* Unknown fields must be skipped, not abort the walk — metadata.proto has
 * ~30 fields on Track and Spotify adds more over time. A track whose
 * duration sits after an unknown field still has to come through. */
static void
test_unknown_fields_are_skipped (void)
{
  GByteArray *track = g_byte_array_new ();

  const gchar *name = "Track With Extras";
  pb_write_bytes_field (track, 2, (const guint8 *) name, strlen (name));

  /* popularity = 8 and has_lyrics = 18: real fields track_meta ignores. */
  pb_write_varint_field (track, 8, 73);
  pb_write_varint_field (track, 18, 1);

  /* A field number well beyond anything in the current schema. */
  pb_write_varint_field (track, 999, 12345);

  pb_write_varint_field (track, 7, zigzag_encode (91000));

  SpotifyTrackMeta meta;
  g_assert_true (spotifygtk_track_meta_parse (track->data, track->len, &meta));

  g_assert_cmpstr (meta.name, ==, "Track With Extras");
  g_assert_cmpint (meta.duration_ms, ==, 91000);

  spotifygtk_track_meta_clear (&meta);
  g_byte_array_free (track, TRUE);
}

/* Regression guard for the bug live data exposed: durations were parsed as
 * plain varints and came out at exactly double. These are real track
 * lengths from the probe run, asserted against the raw bytes the server
 * actually sends. If the zigzag decode is ever dropped, each of these
 * doubles and this fails loudly. */
static void
test_duration_is_zigzag_decoded (void)
{
  const struct { const gchar *what; gint32 ms; } cases[] = {
    { "Never Gonna Give You Up (3:33)", 213000 },
    { "Take on Me (3:45)",              225000 },
    { "ilomilo (2:36)",                 156000 },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
    GByteArray *track = g_byte_array_new ();
    pb_write_varint_field (track, 7, zigzag_encode (cases[i].ms));

    SpotifyTrackMeta meta;
    g_assert_true (spotifygtk_track_meta_parse (track->data, track->len, &meta));

    if (meta.duration_ms != cases[i].ms) {
      g_error ("%s: expected %d ms, got %" G_GINT64_FORMAT " ms (%.2fx) — "
               "duration is almost certainly being read as a plain varint",
               cases[i].what, cases[i].ms, meta.duration_ms,
               (double) meta.duration_ms / cases[i].ms);
    }

    spotifygtk_track_meta_clear (&meta);
    g_byte_array_free (track, TRUE);
  }
}

static void
test_empty_and_garbage (void)
{
  SpotifyTrackMeta meta;

  g_assert_false (spotifygtk_track_meta_parse (NULL, 0, &meta));

  const guint8 empty[1] = { 0 };
  g_assert_false (spotifygtk_track_meta_parse (empty, 0, &meta));

  /* A truncated length-delimited field: claims 200 bytes, supplies none. */
  const guint8 truncated[] = { (2 << 3) | 2, 200 };
  spotifygtk_track_meta_parse (truncated, sizeof truncated, &meta);
  g_assert_null (meta.name);
  spotifygtk_track_meta_clear (&meta);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/track-meta/full-track", test_full_track);
  g_test_add_func ("/track-meta/no-display-fields", test_track_without_display_fields);
  g_test_add_func ("/track-meta/unknown-fields-skipped", test_unknown_fields_are_skipped);
  g_test_add_func ("/track-meta/duration-zigzag", test_duration_is_zigzag_decoded);
  g_test_add_func ("/track-meta/empty-and-garbage", test_empty_and_garbage);

  return g_test_run ();
}

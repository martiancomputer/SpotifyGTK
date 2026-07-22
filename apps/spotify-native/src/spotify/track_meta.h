/*
 * track_meta.h — Display metadata from a metadata.proto `Track` message.
 *
 * Why this exists as its own module: moving the catalog off the Web API
 * means the UI's track titles have to come from somewhere, and that
 * somewhere is the same extended-metadata response spclient.c already
 * fetches for playback. It walks the `Track` message for AudioFile
 * entries (field 12) and throws the rest away; this reads the display
 * fields off that same message.
 *
 * Deliberately split out from spclient.c so it is a pure function over a
 * byte buffer — no session, no network, no GTK — and can therefore be
 * unit-tested against a hand-encoded message rather than only against a
 * live server. See tests/test_track_meta.c.
 *
 * Field numbers transcribed from librespot's
 * protocol/proto/metadata.proto (message Track):
 *
 *   gid = 1 (bytes), name = 2 (string), album = 3 (Album),
 *   artist = 4 (repeated Artist), number = 5, disc_number = 6,
 *   duration = 7 (sint32, milliseconds), file = 12 (repeated AudioFile)
 *
 * Cover art, same file:
 *
 *   Album.cover_group = 17 (ImageGroup)
 *   ImageGroup.image  = 1  (repeated Image)
 *   Image.file_id     = 1 (bytes), size = 2, width = 3, height = 4
 *
 * NOTE: Image.width and Image.height are sint32, so they are zigzag-encoded
 * on the wire exactly like Track.duration was. Reading them as plain varints
 * doubles them.
 *
 * Album and Artist are themselves messages whose own `name` is field 2,
 * per the same file.
 *
 * NOTE ON `duration`: metadata.proto declares it `sint32`, which is
 * zigzag-encoded on the wire, and Spotify does encode it that way.
 * Reading it as a plain varint yields exactly double the real duration
 * (zigzag stores 2n for positive n) -- confirmed against live responses,
 * where every track came back at 2x its true length. See the read site.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gchar   *name;         /* Track.name (field 2) — NULL if absent */
  gchar   *album_name;   /* Track.album.name (3 → 2) — NULL if absent */
  gchar   *artist_names; /* Track.artist[].name (4 → 2), ", "-joined — NULL if none */
  gint64   duration_ms;  /* Track.duration (field 7); 0 if absent */
  gboolean is_explicit;  /* Track.explicit (field 9) */

  /* Navigation targets, as full URIs ready for /context-resolve. Both come
   * from the `gid` (field 1) of the Album / first Artist submessage — a
   * 16-byte value base62-encoded to Spotify's 22-char id, then prefixed.
   * NULL when the submessage carried no gid. Used by the row context menu's
   * "Go to Album" / "Go to Artist". */
  gchar   *album_uri;    /* spotify:album:<id>  — NULL if absent */
  gchar   *artist_uri;   /* spotify:artist:<id> — NULL if absent (first artist) */

  /* Album cover, as lowercase hex of the largest advertised Image.file_id
   * in Track.album.cover_group. NULL when the album carries no artwork.
   *
   * Hex rather than raw bytes because that is the form the image CDN takes
   * (https://i.scdn.co/image/<hex>), so every consumer would otherwise
   * convert it identically. */
  gchar   *cover_id;
} SpotifyTrackMeta;

/*
 * Parse the display fields out of a raw `Track` protobuf message.
 *
 * `out` is filled with newly-allocated strings; free with
 * spotifygtk_track_meta_clear(). Absent fields are left NULL/0 rather
 * than defaulted to placeholder text — rendering a placeholder is the
 * UI's decision, not this layer's.
 *
 * Returns TRUE if the buffer parsed as a well-formed protobuf message.
 * A well-formed message with none of these fields set still returns
 * TRUE with an empty result; malformed wire data returns FALSE.
 */
gboolean spotifygtk_track_meta_parse (const guint8     *track_data,
                                      gsize             track_len,
                                      SpotifyTrackMeta *out);

void spotifygtk_track_meta_clear (SpotifyTrackMeta *meta);

G_END_DECLS

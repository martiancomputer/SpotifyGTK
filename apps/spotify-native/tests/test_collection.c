/*
 * test_collection.c — Byte-exact checks on the collection WriteRequest.
 *
 * The Mercury endpoint for collection writes is unknown (librespot has no
 * write path to copy). That makes it worth pinning the part that *is* known
 * down to the byte: if a live write fails, these tests are what let the
 * failure be attributed to the endpoint rather than to a malformed payload.
 *
 * Expected encodings below are computed by hand from collection2v2.proto
 * rather than captured from the encoder, so they cannot agree with a bug in
 * it -- the same discipline the base62 and duration tests in this suite use.
 */

#include <glib.h>
#include <string.h>

#include "spotify/collection.h"

/* Transport stubs. These tests exercise the encoder, and linking the real
 * Mercury would drag in the AP connection, Shannon and OpenSSL for code that
 * never runs here. spotifygtk_collection_write() is not called by any test;
 * these exist only to satisfy the linker. */
void
spotifygtk_mercury_request (SpotifyMercury *self, MercuryMethod method,
                            const gchar *uri, GBytes *payload,
                            MercuryCallback callback, gpointer user_data)
{
  (void) self; (void) method; (void) uri; (void) payload;
  (void) callback; (void) user_data;
}

GType
spotifygtk_mercury_get_type (void)
{
  return G_TYPE_OBJECT;
}

#define URI_A "spotify:track:4uLU6hMCjMI75M1A2tKUQC"   /* 38 chars */

static void
assert_bytes (GByteArray *got, const guint8 *want, gsize want_len, const gchar *what)
{
  if (got->len != want_len || memcmp (got->data, want, want_len) != 0) {
    g_autoptr(GString) g = g_string_new (NULL);
    g_autoptr(GString) w = g_string_new (NULL);
    for (guint i = 0; i < got->len; i++) g_string_append_printf (g, "%02x", got->data[i]);
    for (gsize  i = 0; i < want_len;  i++) g_string_append_printf (w, "%02x", want[i]);
    g_error ("%s\n  got:  %s\n  want: %s", what, g->str, w->str);
  }
}

/* A like: username, set, one item carrying only its uri. added_at 0 and
 * is_removed FALSE are proto3 defaults and must be absent, not zeroed. */
static void
test_like_encoding (void)
{
  const gchar *uris[] = { URI_A };
  g_autoptr(GByteArray) buf =
    spotifygtk_collection_build_write ("bob", "collection", uris, 1, 0, FALSE);

  guint8 want[128];
  gsize  n = 0;
  want[n++] = 0x0A; want[n++] = 3;                       /* field 1, len 3 */
  memcpy (want + n, "bob", 3);            n += 3;
  want[n++] = 0x12; want[n++] = 10;                      /* field 2, len 10 */
  memcpy (want + n, "collection", 10);    n += 10;
  want[n++] = 0x1A; want[n++] = 2 + strlen (URI_A);      /* field 3, submessage */
  want[n++] = 0x0A; want[n++] = strlen (URI_A);          /*   item field 1 */
  memcpy (want + n, URI_A, strlen (URI_A)); n += strlen (URI_A);

  assert_bytes (buf, want, n, "like encoding");
}

/* An unlike differs by exactly one field: is_removed (3) = true. */
static void
test_unlike_sets_is_removed (void)
{
  const gchar *uris[] = { URI_A };
  g_autoptr(GByteArray) buf =
    spotifygtk_collection_build_write ("bob", "collection", uris, 1, 0, TRUE);

  guint8 want[128];
  gsize  n = 0;
  want[n++] = 0x0A; want[n++] = 3;
  memcpy (want + n, "bob", 3);            n += 3;
  want[n++] = 0x12; want[n++] = 10;
  memcpy (want + n, "collection", 10);    n += 10;
  want[n++] = 0x1A; want[n++] = 2 + strlen (URI_A) + 2;
  want[n++] = 0x0A; want[n++] = strlen (URI_A);
  memcpy (want + n, URI_A, strlen (URI_A)); n += strlen (URI_A);
  want[n++] = 0x18; want[n++] = 0x01;                    /*   is_removed = true */

  assert_bytes (buf, want, n, "unlike encoding");
}

/* added_at is only emitted when non-zero, and as a plain varint (int32 in the
 * schema, so not zigzagged -- the mistake that made track durations come back
 * doubled elsewhere in this project). */
static void
test_added_at_is_plain_varint (void)
{
  const gchar *uris[] = { URI_A };
  g_autoptr(GByteArray) buf =
    spotifygtk_collection_build_write ("bob", "collection", uris, 1, 300, FALSE);

  /* 300 -> varint ac 02. Zigzag would encode it as d8 04, so this also pins
   * that we are not zigzagging. */
  const guint8 marker[] = { 0x10, 0xAC, 0x02 };
  gboolean found = FALSE;
  for (guint i = 0; i + sizeof marker <= buf->len; i++)
    if (memcmp (buf->data + i, marker, sizeof marker) == 0) { found = TRUE; break; }
  g_assert_true (found);
}

/* Several URIs in one request: the repeated field simply recurs. */
static void
test_multiple_items_repeat_the_tag (void)
{
  const gchar *uris[] = { URI_A, URI_A };
  g_autoptr(GByteArray) one =
    spotifygtk_collection_build_write ("bob", "collection", uris, 1, 0, FALSE);
  g_autoptr(GByteArray) two =
    spotifygtk_collection_build_write ("bob", "collection", uris, 2, 0, FALSE);

  /* The second item adds exactly one more tag+len+submessage. */
  g_assert_cmpuint (two->len, ==, one->len + 2 + 2 + strlen (URI_A));
}

/* Empty or NULL URIs are skipped rather than encoded as empty items, so a
 * partially-populated caller array cannot produce a request that asks the
 * server to like nothing. */
static void
test_empty_uris_are_skipped (void)
{
  const gchar *uris[] = { "", NULL, URI_A };
  g_autoptr(GByteArray) buf =
    spotifygtk_collection_build_write ("bob", "collection", uris, 3, 0, FALSE);
  g_autoptr(GByteArray) one_only =
    spotifygtk_collection_build_write ("bob", "collection", (const gchar *const[]) { URI_A }, 1, 0, FALSE);

  g_assert_cmpuint (buf->len, ==, one_only->len);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/collection/like-encoding",      test_like_encoding);
  g_test_add_func ("/collection/unlike-is-removed",  test_unlike_sets_is_removed);
  g_test_add_func ("/collection/added-at-varint",    test_added_at_is_plain_varint);
  g_test_add_func ("/collection/multiple-items",     test_multiple_items_repeat_the_tag);
  g_test_add_func ("/collection/empty-uris-skipped", test_empty_uris_are_skipped);
  return g_test_run ();
}

#include <glib.h>
#include <string.h>
#include "spotify/protobuf_min.h"

static void
test_varint_roundtrip (void)
{
  /* Cover single-byte and multi-byte varint boundaries. */
  guint64 values[] = { 0, 1, 127, 128, 300, 16384, 124200290, G_MAXUINT32 };

  for (gsize i = 0; i < G_N_ELEMENTS (values); i++) {
    GByteArray *buf = g_byte_array_new ();
    pb_write_varint_field (buf, 10, values[i]);

    guint64 got = 0;
    gboolean found = pb_find_varint_field (buf->data, buf->len, 10, &got);

    g_assert_true (found);
    g_assert_cmpuint (got, ==, values[i]);

    g_byte_array_free (buf, TRUE);
  }
}

static void
test_bytes_field_roundtrip (void)
{
  const guint8 payload[] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };

  GByteArray *buf = g_byte_array_new ();
  pb_write_bytes_field (buf, 60, payload, sizeof (payload));

  const guint8 *out_data = NULL;
  gsize out_len = 0;
  gboolean found = pb_find_bytes_field (buf->data, buf->len, 60, &out_data, &out_len);

  g_assert_true (found);
  g_assert_cmpuint (out_len, ==, sizeof (payload));
  g_assert_cmpmem (out_data, out_len, payload, sizeof (payload));

  g_byte_array_free (buf, TRUE);
}

static void
test_nested_message (void)
{
  /* Mirrors the real structure: LoginCryptoDiffieHellmanHello { gc, server_keys_known }
   * wrapped inside LoginCryptoHelloUnion { diffie_hellman } wrapped inside
   * ClientHello { login_crypto_hello }. */
  const guint8 fake_gc[] = { 0x01, 0x02, 0x03, 0x04 };

  GByteArray *dh_hello = g_byte_array_new ();
  pb_write_bytes_field  (dh_hello, 10, fake_gc, sizeof (fake_gc));
  pb_write_varint_field (dh_hello, 20, 1);

  GByteArray *crypto_union = g_byte_array_new ();
  pb_write_message_field (crypto_union, 10, dh_hello->data, dh_hello->len);

  GByteArray *client_hello = g_byte_array_new ();
  pb_write_message_field (client_hello, 50, crypto_union->data, crypto_union->len);

  /* Now decode back down the same path. */
  const guint8 *union_data = NULL; gsize union_len = 0;
  g_assert_true (pb_find_bytes_field (client_hello->data, client_hello->len, 50,
                                      &union_data, &union_len));

  const guint8 *dh_data = NULL; gsize dh_len = 0;
  g_assert_true (pb_find_bytes_field (union_data, union_len, 10, &dh_data, &dh_len));

  const guint8 *gc_data = NULL; gsize gc_len = 0;
  g_assert_true (pb_find_bytes_field (dh_data, dh_len, 10, &gc_data, &gc_len));
  g_assert_cmpmem (gc_data, gc_len, fake_gc, sizeof (fake_gc));

  guint64 server_keys_known = 0;
  g_assert_true (pb_find_varint_field (dh_data, dh_len, 20, &server_keys_known));
  g_assert_cmpuint (server_keys_known, ==, 1);

  g_byte_array_free (dh_hello, TRUE);
  g_byte_array_free (crypto_union, TRUE);
  g_byte_array_free (client_hello, TRUE);
}

static void
test_multibyte_field_tag (void)
{
  /* Field number 80 (feature_set in ClientHello) needs a multi-byte
   * tag varint -- (80 << 3) | 2 = 642, which doesn't fit in 7 bits.
   * This exercises that boundary explicitly. */
  GByteArray *buf = g_byte_array_new ();
  pb_write_varint_field (buf, 80, 42);

  guint64 got = 0;
  g_assert_true (pb_find_varint_field (buf->data, buf->len, 80, &got));
  g_assert_cmpuint (got, ==, 42);

  g_byte_array_free (buf, TRUE);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/protobuf/varint-roundtrip",       test_varint_roundtrip);
  g_test_add_func ("/protobuf/bytes-field-roundtrip",  test_bytes_field_roundtrip);
  g_test_add_func ("/protobuf/nested-message",         test_nested_message);
  g_test_add_func ("/protobuf/multibyte-field-tag",     test_multibyte_field_tag);
  return g_test_run ();
}

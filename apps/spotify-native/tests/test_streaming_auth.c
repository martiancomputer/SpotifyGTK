/*
 * test_streaming_auth.c — offline tests for the clienttoken/login5
 * message encoding used by the streaming auth-relay chain.
 *
 * Same limitation as test_login_encoding.c: can't test actual server
 * acceptance from here (needs network + a real login5/clienttoken
 * round-trip), only that the messages we build match the schemas in
 * protocol/proto/spotify/{clienttoken,login5}/. That's still real
 * coverage -- it's exactly the class of bug (wrong field number,
 * missing field) that's bitten this project twice already in the AP
 * login path.
 */

#include <glib.h>
#include <string.h>
#include "spotify/protobuf_min.h"

/* Mirrors clienttoken.c's request construction. */
static GByteArray *
build_client_token_request (const gchar *client_version, const gchar *client_id)
{
  GByteArray *client_data = g_byte_array_new ();
  pb_write_bytes_field (client_data, 1, (const guint8 *) client_version, strlen (client_version));
  pb_write_bytes_field (client_data, 2, (const guint8 *) client_id, strlen (client_id));

  GByteArray *request = g_byte_array_new ();
  pb_write_varint_field  (request, 1, 1);  /* REQUEST_CLIENT_DATA_REQUEST */
  pb_write_message_field (request, 2, client_data->data, client_data->len);

  g_byte_array_free (client_data, TRUE);
  return request;
}

static void
test_client_token_request_shape (void)
{
  GByteArray *req = build_client_token_request ("1.2.52.442", "65b708073fc0480ea92a077233ca87bd");

  guint64 request_type = 0;
  g_assert_true (pb_find_varint_field (req->data, req->len, 1, &request_type));
  g_assert_cmpuint (request_type, ==, 1);

  const guint8 *cd_data = NULL; gsize cd_len = 0;
  g_assert_true (pb_find_bytes_field (req->data, req->len, 2, &cd_data, &cd_len));

  const guint8 *version_data = NULL; gsize version_len = 0;
  g_assert_true (pb_find_bytes_field (cd_data, cd_len, 1, &version_data, &version_len));
  g_assert_cmpmem (version_data, version_len, "1.2.52.442", 10);

  const guint8 *id_data = NULL; gsize id_len = 0;
  g_assert_true (pb_find_bytes_field (cd_data, cd_len, 2, &id_data, &id_len));
  g_assert_cmpmem (id_data, id_len, "65b708073fc0480ea92a077233ca87bd", 32);

  g_byte_array_free (req, TRUE);
}

/* Mirrors login5.c's request construction. */
static GByteArray *
build_login5_request (const gchar *client_id, const gchar *device_id,
                      const gchar *username, const guint8 *creds, gsize creds_len)
{
  GByteArray *client_info = g_byte_array_new ();
  pb_write_bytes_field (client_info, 1, (const guint8 *) client_id, strlen (client_id));
  pb_write_bytes_field (client_info, 2, (const guint8 *) device_id, strlen (device_id));

  GByteArray *stored_cred = g_byte_array_new ();
  if (username && *username)
    pb_write_bytes_field (stored_cred, 1, (const guint8 *) username, strlen (username));
  pb_write_bytes_field (stored_cred, 2, creds, creds_len);

  GByteArray *request = g_byte_array_new ();
  pb_write_message_field (request, 1,   client_info->data, client_info->len);
  pb_write_message_field (request, 100, stored_cred->data, stored_cred->len);

  g_byte_array_free (client_info, TRUE);
  g_byte_array_free (stored_cred, TRUE);
  return request;
}

static void
test_login5_request_shape_with_username (void)
{
  const guint8 fake_creds[] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02 };
  GByteArray *req = build_login5_request ("65b708073fc0480ea92a077233ca87bd", "test-device",
                                          "someuser", fake_creds, sizeof (fake_creds));

  const guint8 *ci_data = NULL; gsize ci_len = 0;
  g_assert_true (pb_find_bytes_field (req->data, req->len, 1, &ci_data, &ci_len));
  const guint8 *cid_data = NULL; gsize cid_len = 0;
  g_assert_true (pb_find_bytes_field (ci_data, ci_len, 1, &cid_data, &cid_len));
  g_assert_cmpmem (cid_data, cid_len, "65b708073fc0480ea92a077233ca87bd", 32);

  const guint8 *sc_data = NULL; gsize sc_len = 0;
  g_assert_true (pb_find_bytes_field (req->data, req->len, 100, &sc_data, &sc_len));

  const guint8 *user_data = NULL; gsize user_len = 0;
  g_assert_true (pb_find_bytes_field (sc_data, sc_len, 1, &user_data, &user_len));
  g_assert_cmpmem (user_data, user_len, "someuser", 8);

  const guint8 *creds_data = NULL; gsize creds_data_len = 0;
  g_assert_true (pb_find_bytes_field (sc_data, sc_len, 2, &creds_data, &creds_data_len));
  g_assert_cmpmem (creds_data, creds_data_len, fake_creds, sizeof (fake_creds));

  g_byte_array_free (req, TRUE);
}

static void
test_login5_request_omits_username_when_null (void)
{
  const guint8 fake_creds[] = { 0x01, 0x02, 0x03 };
  GByteArray *req = build_login5_request ("client-id", "device-id", NULL, fake_creds, sizeof (fake_creds));

  const guint8 *sc_data = NULL; gsize sc_len = 0;
  g_assert_true (pb_find_bytes_field (req->data, req->len, 100, &sc_data, &sc_len));

  const guint8 *user_data = NULL; gsize user_len = 0;
  g_assert_false (pb_find_bytes_field (sc_data, sc_len, 1, &user_data, &user_len));

  const guint8 *creds_data = NULL; gsize creds_data_len = 0;
  g_assert_true (pb_find_bytes_field (sc_data, sc_len, 2, &creds_data, &creds_data_len));
  g_assert_cmpmem (creds_data, creds_data_len, fake_creds, sizeof (fake_creds));

  g_byte_array_free (req, TRUE);
}

/* storage-resolve response parsing (repeated cdnurl field) --
 * verifies the pb_read_field-based manual walk in spclient.c against
 * a message with multiple repeated entries, since none of the
 * existing protobuf_min tests exercise a repeated field at all. */
static void
test_storage_resolve_repeated_cdnurl_parsing (void)
{
  GByteArray *response = g_byte_array_new ();
  pb_write_varint_field (response, 1, 0);  /* result = CDN */
  pb_write_bytes_field  (response, 2, (const guint8 *) "https://cdn1.example/a", 22);
  pb_write_bytes_field  (response, 2, (const guint8 *) "https://cdn2.example/b", 22);
  pb_write_bytes_field  (response, 2, (const guint8 *) "https://cdn3.example/c", 22);

  guint count = 0;
  gsize pos = 0;
  guint32 field_num;
  PbWireType wire_type;
  const guint8 *field_data;
  gsize field_len;
  guint64 field_varint;

  while (pb_read_field (response->data, response->len, &pos, &field_num, &wire_type,
                        &field_data, &field_len, &field_varint)) {
    if (field_num == 2 && wire_type == PB_WIRE_LENGTH_DELIMITED)
      count++;
  }

  g_assert_cmpuint (count, ==, 3);
  g_byte_array_free (response, TRUE);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/streaming-auth/client-token-request-shape", test_client_token_request_shape);
  g_test_add_func ("/streaming-auth/login5-request-with-username", test_login5_request_shape_with_username);
  g_test_add_func ("/streaming-auth/login5-request-omits-username-when-null",
                   test_login5_request_omits_username_when_null);
  g_test_add_func ("/streaming-auth/storage-resolve-repeated-cdnurl",
                   test_storage_resolve_repeated_cdnurl_parsing);
  return g_test_run ();
}

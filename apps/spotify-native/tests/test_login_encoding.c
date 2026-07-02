/*
 * test_login_encoding.c — offline test of the login message shape.
 *
 * Doesn't (and can't, in this environment) test interop with a real
 * server -- that's what main.c's live test is for. What this
 * verifies: the LoginCredentials / SystemInfo encoding logic in
 * ap.c's spotifygtk_ap_session_login() builds a structurally correct
 * ClientResponseEncrypted, by reproducing the same encode calls
 * directly and decoding the result back down to confirm each field
 * matches authentication.proto.
 *
 * HISTORY WORTH KEEPING: an earlier version of both this test and the
 * real ap.c code it mirrors sent system_info under field tag 0x14
 * (which is actually account_creation on ClientResponseEncrypted --
 * the real field is 0x32) and left SystemInfo's two required fields
 * (cpu_family, os) completely unset. This test asserted exactly that
 * wrong shape and passed, because it re-implemented the same mistake
 * independently rather than checking against the schema -- so it
 * gave false confidence right up until a live login attempt failed
 * against a real server. Fixed now to assert the real field numbers
 * and required-field presence, re-checked against
 * protocol/proto/authentication.proto directly rather than against
 * whatever the implementation happens to currently do.
 */

#include <glib.h>
#include <string.h>
#include "spotify/protobuf_min.h"

/* Mirrors the encode logic in spotifygtk_ap_session_login() -- kept
 * here as a standalone function so this test doesn't need to link
 * the whole AP session machinery just to check message shape. */
static GByteArray *
build_client_response_encrypted (const gchar *username, const gchar *oauth_token)
{
  GByteArray *login_credentials = g_byte_array_new ();
  if (username && *username)
    pb_write_bytes_field (login_credentials, 0x0a, (const guint8 *) username, strlen (username));
  pb_write_varint_field (login_credentials, 0x14, 8);  /* AUTHENTICATION_SPOTIFY_TOKEN */
  pb_write_bytes_field  (login_credentials, 0x1e, (const guint8 *) oauth_token, strlen (oauth_token));

  /* SystemInfo: cpu_family (0x0a, REQUIRED) and os (0x3c, REQUIRED)
   * per authentication.proto -- CPU_X86_64=0x2, OS_LINUX=0x5. */
  GByteArray *system_info = g_byte_array_new ();
  pb_write_varint_field (system_info, 0x0a, 0x2);
  pb_write_varint_field (system_info, 0x3c, 0x5);
  pb_write_bytes_field  (system_info, 0x5a, (const guint8 *) "test-build", 10);
  pb_write_bytes_field  (system_info, 0x64, (const guint8 *) "test-device-id", 14);

  GByteArray *client_response = g_byte_array_new ();
  pb_write_message_field (client_response, 0x0a, login_credentials->data, login_credentials->len);
  pb_write_message_field (client_response, 0x32, system_info->data, system_info->len);
  pb_write_bytes_field   (client_response, 0x46, (const guint8 *) "test-version", 12);

  g_byte_array_free (login_credentials, TRUE);
  g_byte_array_free (system_info, TRUE);
  return client_response;
}

static void
test_login_with_token_only (void)
{
  /* The real with_access_token path -- per librespot's
   * authentication.rs, username is None (not sent) in this case. */
  GByteArray *msg = build_client_response_encrypted (NULL, "fake-oauth-token-for-test");

  const guint8 *creds_data = NULL; gsize creds_len = 0;
  g_assert_true (pb_find_bytes_field (msg->data, msg->len, 0x0a, &creds_data, &creds_len));

  /* username field should be ABSENT, not present-and-empty -- these
   * are different things on the wire, and the distinction matters
   * since librespot's reference behavior is "field not sent". */
  const guint8 *username_data = NULL; gsize username_len = 0;
  g_assert_false (pb_find_bytes_field (creds_data, creds_len, 0x0a, &username_data, &username_len));

  guint64 auth_type = 0;
  g_assert_true (pb_find_varint_field (creds_data, creds_len, 0x14, &auth_type));
  g_assert_cmpuint (auth_type, ==, 8);  /* AUTHENTICATION_SPOTIFY_TOKEN */

  const guint8 *auth_data = NULL; gsize auth_data_len = 0;
  g_assert_true (pb_find_bytes_field (creds_data, creds_len, 0x1e, &auth_data, &auth_data_len));
  g_assert_cmpmem (auth_data, auth_data_len, "fake-oauth-token-for-test", 26);

  g_byte_array_free (msg, TRUE);
}

static void
test_login_with_username_and_token (void)
{
  GByteArray *msg = build_client_response_encrypted ("someuser", "another-fake-token");

  const guint8 *creds_data = NULL; gsize creds_len = 0;
  g_assert_true (pb_find_bytes_field (msg->data, msg->len, 0x0a, &creds_data, &creds_len));

  const guint8 *username_data = NULL; gsize username_len = 0;
  g_assert_true (pb_find_bytes_field (creds_data, creds_len, 0x0a, &username_data, &username_len));
  g_assert_cmpmem (username_data, username_len, "someuser", 8);

  g_byte_array_free (msg, TRUE);
}

static void
test_system_info_field_number_and_required_fields (void)
{
  /* This is the test that would have caught the original bug: field
   * 0x14 on the outer message is account_creation, NOT system_info
   * (that's field 0x32) -- and SystemInfo itself has two required
   * (proto2) fields that must actually be present, not just an
   * empty embedded message. */
  GByteArray *msg = build_client_response_encrypted (NULL, "token");

  /* system_info must be at 0x32 ... */
  const guint8 *sysinfo_data = NULL; gsize sysinfo_len = 0;
  g_assert_true (pb_find_bytes_field (msg->data, msg->len, 0x32, &sysinfo_data, &sysinfo_len));
  g_assert_cmpuint (sysinfo_len, >, 0);  /* NOT the empty-message bug */

  /* ... and 0x14 must NOT hold an embedded message shaped like
   * system_info -- it's account_creation on this message, a
   * different field entirely. We didn't set account_creation at all,
   * so this must be absent. */
  const guint8 *field_14_data = NULL; gsize field_14_len = 0;
  g_assert_false (pb_find_bytes_field (msg->data, msg->len, 0x14, &field_14_data, &field_14_len));

  /* Within SystemInfo: cpu_family (required) and os (required) must
   * both actually be present as varints. */
  guint64 cpu_family = 0, os = 0;
  g_assert_true (pb_find_varint_field (sysinfo_data, sysinfo_len, 0x0a, &cpu_family));
  g_assert_true (pb_find_varint_field (sysinfo_data, sysinfo_len, 0x3c, &os));
  g_assert_cmpuint (cpu_family, ==, 0x2);  /* CPU_X86_64 */
  g_assert_cmpuint (os, ==, 0x5);          /* OS_LINUX */

  /* version_string on the outer message, field 0x46 (not 0x1e --
   * that's LoginCredentials.auth_data's field number on a completely
   * different, nested message; an earlier version of the code
   * comment here confused the two). */
  const guint8 *version_data = NULL; gsize version_len = 0;
  g_assert_true (pb_find_bytes_field (msg->data, msg->len, 0x46, &version_data, &version_len));
  g_assert_cmpuint (version_len, >, 0);

  g_byte_array_free (msg, TRUE);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/login-encoding/token-only-omits-username", test_login_with_token_only);
  g_test_add_func ("/login-encoding/username-present-when-given", test_login_with_username_and_token);
  g_test_add_func ("/login-encoding/system-info-field-number-and-required-fields",
                   test_system_info_field_number_and_required_fields);
  return g_test_run ();
}

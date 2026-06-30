/*
 * test_login_encoding.c — offline test of the login message shape.
 *
 * Doesn't (and can't, in this environment) test interop with a real
 * server -- that's what main.c's SPOTIFY_ACCESS_TOKEN-gated live test
 * is for. What this verifies: the LoginCredentials / SystemInfo
 * encoding logic in ap.c's spotifygtk_ap_session_login() builds a
 * structurally correct ClientResponseEncrypted, by reproducing the
 * same encode calls directly (rather than going through the full
 * SpotifyApSession/network machinery) and decoding the result back
 * down to confirm each field round-trips per authentication.proto.
 */

#include <glib.h>
#include <string.h>
#include "spotify/protobuf_min.h"

/* Mirrors the encode logic in spotifygtk_ap_session_login() exactly --
 * kept here as a standalone function so this test doesn't need to
 * link the whole AP session machinery (sockets, Shannon ciphers) just
 * to check message shape. If the real function's encoding changes,
 * this should be updated to match, the same way the test would be the
 * thing that catches an accidental divergence. */
static GByteArray *
build_client_response_encrypted (const gchar *username, const gchar *oauth_token)
{
  GByteArray *login_credentials = g_byte_array_new ();
  if (username && *username)
    pb_write_bytes_field (login_credentials, 0x0a, (const guint8 *) username, strlen (username));
  pb_write_varint_field (login_credentials, 0x14, 8);  /* AUTHENTICATION_SPOTIFY_TOKEN */
  pb_write_bytes_field  (login_credentials, 0x1e, (const guint8 *) oauth_token, strlen (oauth_token));

  GByteArray *client_response = g_byte_array_new ();
  pb_write_message_field (client_response, 0x0a, login_credentials->data, login_credentials->len);
  pb_write_message_field (client_response, 0x14, NULL, 0);

  g_byte_array_free (login_credentials, TRUE);
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

  /* system_info (field 0x14 on the outer message) should be present
   * as an embedded message, even though empty. */
  const guint8 *sysinfo_data = NULL; gsize sysinfo_len = 0;
  g_assert_true (pb_find_bytes_field (msg->data, msg->len, 0x14, &sysinfo_data, &sysinfo_len));
  g_assert_cmpuint (sysinfo_len, ==, 0);

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

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/login-encoding/token-only-omits-username", test_login_with_token_only);
  g_test_add_func ("/login-encoding/username-present-when-given", test_login_with_username_and_token);
  return g_test_run ();
}

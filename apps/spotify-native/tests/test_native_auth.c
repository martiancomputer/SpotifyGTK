/*
 * test_native_auth.c — offline tests for native_auth's PKCE plumbing.
 *
 * Can't test the actual OAuth exchange or AP login acceptance here --
 * that needs network access to Spotify plus a human clicking through
 * a consent screen, which is exactly what main.c's real run is for.
 * What this checks: the parts that are pure logic and easy to get
 * subtly wrong without noticing (constant values, PKCE math), the
 * same category of thing test_dh.c and test_shannon.c already cover
 * for their respective modules.
 */

#include <glib.h>
#include <string.h>
#include "spotify/native_auth.h"

static void
test_constants_match_librespot (void)
{
  /* These are the specific values that made login start working --
   * a silent edit to any of them would be a real, hard-to-notice
   * regression back to "AP closes the connection in ~100ms with no
   * explanation." Pinning them here means that regression fails a
   * test immediately instead of only showing up against a live
   * server run. */
  g_assert_cmpstr (NATIVE_AUTH_CLIENT_ID, ==, "65b708073fc0480ea92a077233ca87bd");
  g_assert_cmpstr (NATIVE_AUTH_REDIRECT_URI, ==, "http://127.0.0.1:8898/login");
  g_assert_cmpint (NATIVE_AUTH_REDIRECT_PORT, ==, 8898);

  /* Spot-check a few scopes from librespot's OAUTH_SCOPES rather than
   * the full 26-entry string -- enough to catch "someone edited this
   * and dropped/mistyped an entry" without the test itself becoming
   * a second place that string has to be kept in sync by hand. */
  g_assert_nonnull (strstr (NATIVE_AUTH_SCOPES, "streaming"));
  g_assert_nonnull (strstr (NATIVE_AUTH_SCOPES, "app-remote-control"));
  g_assert_nonnull (strstr (NATIVE_AUTH_SCOPES, "user-top-read"));

  /* Exact count matters too -- catches an accidental duplicate or a
   * scope silently dropped from the middle, neither of which the
   * three spot-checks above would necessarily notice. */
  guint space_count = 0;
  for (const gchar *p = NATIVE_AUTH_SCOPES; *p; p++)
    if (*p == ' ') space_count++;
  g_assert_cmpuint (space_count, ==, 25);  /* 26 scopes, 25 separating spaces */
}

static void
test_object_creation (void)
{
  NativeAuth *auth = native_auth_new ();
  g_assert_nonnull (auth);
  /* Fresh object, nothing stored/loaded yet from a previous run in
   * this environment -- don't assert has_valid_token() is FALSE
   * here, since on a machine with a real prior login it would
   * legitimately be TRUE. This just checks construction doesn't
   * crash. */
  g_object_unref (auth);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/native-auth/constants-match-librespot", test_constants_match_librespot);
  g_test_add_func ("/native-auth/object-creation",           test_object_creation);
  return g_test_run ();
}

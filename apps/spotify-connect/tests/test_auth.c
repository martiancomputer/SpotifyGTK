#include <glib.h>
#include "auth.h"

static void
test_auth_creation (void)
{
  SpotifyAuth *auth = spotifygtk_auth_new ();
  g_assert_nonnull (auth);
  g_assert_false (spotifygtk_auth_has_valid_token (auth));
  g_object_unref (auth);
}

static void
test_auth_no_token_without_env (void)
{
  g_unsetenv ("SPOTIFY_CLIENT_ID");
  SpotifyAuth *auth = spotifygtk_auth_new ();
  g_assert_false (spotifygtk_auth_has_valid_token (auth));
  g_object_unref (auth);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/auth/creation",        test_auth_creation);
  g_test_add_func ("/auth/no-token-no-env", test_auth_no_token_without_env);
  return g_test_run ();
}

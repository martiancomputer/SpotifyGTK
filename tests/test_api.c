#include <glib.h>
#include "auth.h"
#include "api.h"

static void
test_api_creation (void)
{
  SpotifyAuth *auth = spotifygtk_auth_new ();
  SpotifyApi  *api  = spotifygtk_api_new (auth);
  g_assert_nonnull (api);
  g_object_unref (api);
  g_object_unref (auth);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/api/creation", test_api_creation);
  return g_test_run ();
}

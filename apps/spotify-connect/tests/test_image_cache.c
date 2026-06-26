#include <glib.h>
#include "image_cache.h"

static void
test_image_cache_creation (void)
{
  SpotifyImageCache *cache = spotifygtk_image_cache_new ();
  g_assert_nonnull (cache);
  g_assert_nonnull (spotifygtk_image_cache_active_backend (cache));
  g_object_unref (cache);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/image-cache/creation", test_image_cache_creation);
  return g_test_run ();
}

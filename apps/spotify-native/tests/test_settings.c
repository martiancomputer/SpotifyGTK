#include <glib.h>

#include "ui/settings.h"

static void
test_renderer_backend (void)
{
  g_assert_null (spotifygtk_renderer_backend (SPOTIFYGTK_RENDERER_AUTOMATIC));
  g_assert_cmpstr (spotifygtk_renderer_backend (SPOTIFYGTK_RENDERER_VULKAN),
                   ==, "vulkan");
  g_assert_cmpstr (spotifygtk_renderer_backend (SPOTIFYGTK_RENDERER_OPENGL),
                   ==, "opengl");
  g_assert_cmpstr (spotifygtk_renderer_backend (SPOTIFYGTK_RENDERER_CAIRO),
                   ==, "cairo");
  g_assert_null (spotifygtk_renderer_backend ((SpotifyGtkRenderer) 99));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/settings/renderer-backend", test_renderer_backend);
  return g_test_run ();
}

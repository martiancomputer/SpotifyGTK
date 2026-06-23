#include "config.h"
#include "app.h"

int
main (int argc, char *argv[])
{
  g_set_application_name ("SpotifyGTK");
  g_set_prgname ("spotifygtk");

  SpotifyGtkApp *app = spotifygtk_app_new ();
  int ret = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  return ret;
}

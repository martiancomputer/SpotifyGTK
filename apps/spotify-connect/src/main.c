#include "config.h"
#include "app.h"

int
main (int argc, char *argv[])
{
  g_set_application_name ("Spotify Connect");
  g_set_prgname ("spotify-connect");

  SpotifyGtkApp *app = spotifygtk_app_new ();
  int ret = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  return ret;
}

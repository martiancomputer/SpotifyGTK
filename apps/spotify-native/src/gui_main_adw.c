/*
 * gui_main_adw.c — Entry point for the libadwaita SpotifyGTK Native shell.
 */

#include "config.h"
#include "ui/window.h"

static void
on_activate (GtkApplication *app, gpointer user_data)
{
  adw_init ();

  SpotifyGtkNativeWindow *win = spotifygtk_native_window_new (app);
  gtk_window_present (GTK_WINDOW (win));
  (void) user_data;
}

int
main (int argc, char *argv[])
{
  GtkApplication *app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}

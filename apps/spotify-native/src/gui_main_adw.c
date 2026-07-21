/*
 * gui_main_adw.c — Entry point for the libadwaita SpotifyGTK Native shell.
 */

#include "config.h"
#include "ui/window.h"

static void
on_activate (GtkApplication *app, gpointer user_data)
{
  adw_init ();

  /* GtkApplication is single-instance, so launching again while one is
   * running routes here rather than starting a new process. Building a
   * second window each time meant a second launch silently stacked another
   * copy of the whole UI -- including a second sign-in -- instead of
   * raising the one already open. */
  GtkWindow *existing = gtk_application_get_active_window (app);
  if (existing) {
    gtk_window_present (existing);
    (void) user_data;
    return;
  }

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

/*
 * gui_main_adw.c — Entry point for the libadwaita SpotifyGTK Native shell.
 */

#include "config.h"
#include "ui/window.h"
#include "playback_probe.h"
#include "log_file.h"

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
  /* Before anything else that might log, so a bug report covers sign-in and
   * the AP handshake rather than starting halfway through the session. */
  spotifygtk_log_file_init ();

  /* Single-instance is right for users but blocks running a freshly built
   * copy while another is already open, which makes verifying UI changes
   * awkward. SPOTIFY_DEV_INSTANCE opts out for development. */
  GApplicationFlags flags = G_APPLICATION_DEFAULT_FLAGS;
  const gchar *dev_instance = g_getenv ("SPOTIFY_DEV_INSTANCE");
  if (dev_instance && *dev_instance) {
    flags |= G_APPLICATION_NON_UNIQUE;
    g_message ("running as a non-unique dev instance");
  }

  /* Diagnostic path: drives player_service directly with no window, which
   * is the only way to exercise it — it is not linked into the harness. */
  const gchar *playback_probe = g_getenv ("SPOTIFY_PROBE_PLAYBACK");
  if (playback_probe && *playback_probe) {
    gtk_init ();
    return spotifygtk_run_playback_probe (playback_probe);
  }

  GtkApplication *app = gtk_application_new (APP_ID, flags);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  spotifygtk_log_file_shutdown ();
  return status;
}

#include "frame_policy.h"
#include "presentation_policy.h"
#include "settings.h"

#define POLICY_VERSION_KEY "gtk4-enhanced-frame-policy-version"
#define POLICY_LIMIT_KEY "gtk4-enhanced-max-fps-v1"

gboolean
spotifygtk_frame_policy_supported (GtkWidget *widget)
{
  GdkFrameClock *clock = gtk_widget_get_frame_clock (widget);
  return clock && GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (clock),
                                                    POLICY_VERSION_KEY)) == 1;
}

static void
apply_policy (GtkWidget *window)
{
  if (!gtk_widget_get_realized (window) || !spotifygtk_frame_policy_supported (window))
    return;

  GdkFrameClock *clock = gtk_widget_get_frame_clock (window);
  guint fps = spotifygtk_settings_get_frame_rate (spotifygtk_settings_get_default ());
  if (!spotifygtk_presentation_valid_fps (fps)) fps = 0;
  if (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (clock), POLICY_LIMIT_KEY)) == fps)
    return;
  g_object_set_data (G_OBJECT (clock), POLICY_LIMIT_KEY, GUINT_TO_POINTER (fps));
  /* Wake only for this setting change. GDK owns pacing and cancellation; no
   * recurring app timer, forced tick callback, or snapshot withholding. */
  gtk_widget_queue_draw (window);
}

static void
on_realize (GtkWidget *window, gpointer data)
{
  apply_policy (window);
  (void) data;
}

static void
on_settings_changed (SpotifyGtkSettings *settings, GtkWidget *window)
{
  apply_policy (window);
  (void) settings;
}

void
spotifygtk_frame_policy_bind (GtkWidget *window)
{
  g_return_if_fail (GTK_IS_WINDOW (window));
  if (g_object_get_data (G_OBJECT (window), "frame-policy-bound")) return;
  g_object_set_data (G_OBJECT (window), "frame-policy-bound", GINT_TO_POINTER (1));
  g_signal_connect (window, "realize", G_CALLBACK (on_realize), NULL);
  g_signal_connect_object (spotifygtk_settings_get_default (), "changed",
                          G_CALLBACK (on_settings_changed), window, 0);
  apply_policy (window);
}

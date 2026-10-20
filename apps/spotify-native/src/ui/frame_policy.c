#include "frame_policy.h"
#include "presentation_policy.h"
#include "settings.h"

#define POLICY_VERSION_KEY "gtk4-enhanced-frame-policy-version"
#define POLICY_LIMIT_KEY "gtk4-enhanced-max-fps-v1"
#define UNSYNC_VERSION_KEY "gtk4-enhanced-unsynchronized-version"
#define UNSYNC_REQUEST_KEY "gtk4-enhanced-unsynchronized-v1"
#define UNSYNC_AVAILABLE_KEY "gtk4-enhanced-unsynchronized-available-v1"

gboolean
spotifygtk_frame_policy_supported (GtkWidget *widget)
{
  GdkFrameClock *clock = gtk_widget_get_frame_clock (widget);
  return clock && GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (clock),
                                                    POLICY_VERSION_KEY)) == 1;
}

gboolean
spotifygtk_frame_policy_unsync_supported (GtkWidget *widget)
{
  GdkFrameClock *clock = gtk_widget_get_frame_clock (widget);
  GtkNative *native = gtk_widget_get_native (widget);
  GskRenderer *renderer = native ? gtk_native_get_renderer (native) : NULL;
  return clock && renderer &&
    GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (clock), UNSYNC_VERSION_KEY)) == 1 &&
    g_str_equal (G_OBJECT_TYPE_NAME (gtk_widget_get_display (widget)), "GdkWaylandDisplay") &&
    g_str_equal (G_OBJECT_TYPE_NAME (renderer), "GskVulkanRenderer");
}

gboolean
spotifygtk_frame_policy_unsync_available (GtkWidget *widget)
{
  return spotifygtk_frame_policy_unsync_supported (widget) &&
    GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (gtk_widget_get_frame_clock (widget)),
                                       UNSYNC_AVAILABLE_KEY)) == 1;
}

static void
apply_policy (GtkWidget *window)
{
  if (!gtk_widget_get_realized (window) || !spotifygtk_frame_policy_supported (window))
    return;

  GdkFrameClock *clock = gtk_widget_get_frame_clock (window);
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  guint fps = spotifygtk_settings_get_frame_rate (settings);
  if (!spotifygtk_presentation_valid_fps (fps)) fps = 0;
  gboolean unsynchronized = fps != 0 &&
    spotifygtk_settings_get_unsynchronized_rendering (settings) &&
    spotifygtk_frame_policy_unsync_supported (window) &&
    gtk_window_is_active (GTK_WINDOW (window)) && gtk_widget_get_visible (window);
  if (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (clock), POLICY_LIMIT_KEY)) == fps &&
      GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (clock), UNSYNC_REQUEST_KEY)) == (guint) unsynchronized)
    return;
  g_object_set_data (G_OBJECT (clock), POLICY_LIMIT_KEY, GUINT_TO_POINTER (fps));
  g_object_set_data (G_OBJECT (clock), UNSYNC_REQUEST_KEY, GUINT_TO_POINTER (unsynchronized));
  if (g_getenv ("SPOTIFYGTK_FRAME_POLICY_DIAGNOSTICS"))
    g_message ("frame-policy: limit=%u unsynchronized-request=%d available=%d focused=%d",
      fps, unsynchronized, spotifygtk_frame_policy_unsync_available (window),
      gtk_window_is_active (GTK_WINDOW (window)));
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

static void
on_window_state_changed (GtkWidget *window, GParamSpec *pspec, gpointer unused)
{
  (void) pspec; (void) unused;
  apply_policy (window);
}

void
spotifygtk_frame_policy_bind (GtkWidget *window)
{
  g_return_if_fail (GTK_IS_WINDOW (window));
  if (g_object_get_data (G_OBJECT (window), "frame-policy-bound")) return;
  g_object_set_data (G_OBJECT (window), "frame-policy-bound", GINT_TO_POINTER (1));
  g_signal_connect (window, "realize", G_CALLBACK (on_realize), NULL);
  g_signal_connect (window, "notify::is-active", G_CALLBACK (on_window_state_changed), NULL);
  g_signal_connect (window, "notify::visible", G_CALLBACK (on_window_state_changed), NULL);
  g_signal_connect_object (spotifygtk_settings_get_default (), "changed",
                          G_CALLBACK (on_settings_changed), window, 0);
  apply_policy (window);
}

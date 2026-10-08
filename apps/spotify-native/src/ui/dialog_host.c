#include "dialog_host.h"

void
spotifygtk_dialog_prepare (AdwDialog *dialog)
{
  g_return_if_fail (ADW_IS_DIALOG (dialog));
  gtk_widget_add_css_class (GTK_WIDGET (dialog), "spotifygtk-dialog");
  adw_dialog_set_presentation_mode (dialog, ADW_DIALOG_FLOATING);
  adw_dialog_set_can_close (dialog, TRUE);
}

static void
on_backdrop_pressed (GtkGestureClick *gesture, gint n_press, gdouble x,
                     gdouble y, AdwApplicationWindow *window)
{
  (void) n_press;
  AdwDialog *dialog = adw_application_window_get_visible_dialog (window);
  if (!dialog) return;
  /* AdwDialog itself spans the entire host, including the dimming layer.
   * Walk outward from its public content to the centered surface, stopping
   * at the first full-host container. Include the surface's blank padding,
   * not just its buttons and entry, without relying on private CSS names. */
  GtkWidget *surface = adw_dialog_get_child (dialog);
  if (!surface) return;
  for (GtkWidget *parent = gtk_widget_get_parent (surface);
       parent && parent != GTK_WIDGET (dialog);
       parent = gtk_widget_get_parent (parent)) {
    if (gtk_widget_get_width (parent) >= gtk_widget_get_width (GTK_WIDGET (dialog)) &&
        gtk_widget_get_height (parent) >= gtk_widget_get_height (GTK_WIDGET (dialog)))
      break;
    surface = parent;
  }
  GtkWidget *picked = gtk_widget_pick (GTK_WIDGET (window), x, y, GTK_PICK_DEFAULT);
  if (!picked || picked == surface || gtk_widget_is_ancestor (picked, surface)) return;
  /* Claim the whole sequence before closing: its release must not activate
   * a control underneath the dismissed sheet, or start a window drag. */
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  adw_dialog_close (dialog);
}

static gboolean
on_dialog_escape (GtkEventControllerKey *controller, guint keyval, guint keycode,
                  GdkModifierType state, AdwApplicationWindow *window)
{
  (void) controller; (void) keycode; (void) state;
  AdwDialog *dialog = adw_application_window_get_visible_dialog (window);
  if (keyval != GDK_KEY_Escape || !dialog) return GDK_EVENT_PROPAGATE;
  adw_dialog_close (dialog);
  return GDK_EVENT_STOP;
}

void
spotifygtk_dialog_host_bind (AdwApplicationWindow *window)
{
  g_return_if_fail (ADW_IS_APPLICATION_WINDOW (window));
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_backdrop_pressed), window);
  gtk_widget_add_controller (GTK_WIDGET (window), GTK_EVENT_CONTROLLER (click));
  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_dialog_escape), window);
  gtk_widget_add_controller (GTK_WIDGET (window), keys);
}

#include "dialog_host.h"

void
spotifygtk_dialog_prepare (AdwDialog *dialog)
{
  g_return_if_fail (ADW_IS_DIALOG (dialog));
  gtk_widget_add_css_class (GTK_WIDGET (dialog), "spotifygtk-dialog");
  adw_dialog_set_presentation_mode (dialog, ADW_DIALOG_FLOATING);
  adw_dialog_set_can_close (dialog, TRUE);
}

static gboolean
is_backdrop (AdwApplicationWindow *window, gdouble x, gdouble y)
{
  AdwDialog *dialog = adw_application_window_get_visible_dialog (window);
  if (!dialog) return FALSE;
  /* AdwDialog itself spans the entire host, including the dimming layer.
   * Walk outward from its public content to the centered surface, stopping
   * at the first full-host container. Include the surface's blank padding,
   * not just its buttons and entry, without relying on private CSS names. */
  GtkWidget *surface = adw_dialog_get_child (dialog);
  if (!surface) return FALSE;
  for (GtkWidget *parent = gtk_widget_get_parent (surface);
       parent && parent != GTK_WIDGET (dialog);
       parent = gtk_widget_get_parent (parent)) {
    /* sheet is AdwDialog's documented CSS surface, including its padding.
     * Stop here rather than mistaking a full-host wrapper for its content. */
    if (g_str_equal (gtk_widget_get_css_name (parent), "sheet")) {
      surface = parent;
      break;
    }
    if (gtk_widget_get_width (parent) >= gtk_widget_get_width (GTK_WIDGET (dialog)) &&
        gtk_widget_get_height (parent) >= gtk_widget_get_height (GTK_WIDGET (dialog)))
      break;
    surface = parent;
  }
  graphene_rect_t bounds;
  if (!gtk_widget_compute_bounds (surface, GTK_WIDGET (window), &bounds)) return FALSE;
  graphene_point_t point = GRAPHENE_POINT_INIT (x, y);
  return !graphene_rect_contains_point (&bounds, &point);
}

static void
on_backdrop_pressed (GtkGestureClick *gesture, gint n_press, gdouble x,
                     gdouble y, AdwApplicationWindow *window)
{
  (void) n_press;
  GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  graphene_point_t local = GRAPHENE_POINT_INIT (x, y), point;
  if (!gtk_widget_compute_point (widget, GTK_WIDGET (window), &local, &point)) return;
  x = point.x; y = point.y;
  if (!is_backdrop (window, x, y)) return;
  /* Claim the whole sequence before closing: its release must not activate
   * a control underneath the dismissed sheet, or start a window drag. */
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  adw_dialog_close (adw_application_window_get_visible_dialog (window));
}

static gboolean
on_backdrop_event (GtkEventControllerLegacy *controller, GdkEvent *event,
                   AdwApplicationWindow *window)
{
  (void) controller;
  GdkEventType type = gdk_event_get_event_type (event);
  gboolean claimed = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "dialog-backdrop-sequence"));
  if (claimed) {
    if (type == GDK_BUTTON_RELEASE || type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL)
      g_object_set_data (G_OBJECT (window), "dialog-backdrop-sequence", NULL);
    if (type == GDK_BUTTON_RELEASE || type == GDK_MOTION_NOTIFY ||
        type == GDK_TOUCH_UPDATE || type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL)
      return GDK_EVENT_STOP;
  }
  if (type != GDK_TOUCH_BEGIN && (type != GDK_BUTTON_PRESS ||
      gdk_button_event_get_button (event) != GDK_BUTTON_PRIMARY)) return GDK_EVENT_PROPAGATE;
  gdouble x, y, dx, dy;
  if (!gdk_event_get_position (event, &x, &y)) return GDK_EVENT_PROPAGATE;
  gtk_native_get_surface_transform (GTK_NATIVE (window), &dx, &dy);
  if (!is_backdrop (window, x + dx, y + dy)) return GDK_EVENT_PROPAGATE;
  /* Capture the raw event before GtkWindowHandle's drag gesture on the
   * dimming layer can claim it. GestureClick alone can lose that sequence.
   * Consume release too, so dismissal cannot activate a background button. */
  g_object_set_data (G_OBJECT (window), "dialog-backdrop-sequence", GINT_TO_POINTER (1));
  adw_dialog_close (adw_application_window_get_visible_dialog (window));
  return GDK_EVENT_STOP;
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

static void
bind_controllers (GtkWidget *widget, AdwApplicationWindow *window)
{
  GtkEventController *events = gtk_event_controller_legacy_new ();
  gtk_event_controller_set_propagation_phase (events, GTK_PHASE_CAPTURE);
  g_signal_connect_object (events, "event", G_CALLBACK (on_backdrop_event), window, 0);
  gtk_widget_add_controller (widget, events);
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
  g_signal_connect_object (click, "pressed", G_CALLBACK (on_backdrop_pressed), window, 0);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (click));
  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect_object (keys, "key-pressed", G_CALLBACK (on_dialog_escape), window, 0);
  gtk_widget_add_controller (widget, keys);
}

static void
on_visible_dialog (AdwApplicationWindow *window, GParamSpec *pspec, gpointer data)
{
  (void) pspec; (void) data;
  AdwDialog *dialog = adw_application_window_get_visible_dialog (window);
  if (!dialog || g_object_get_data (G_OBJECT (dialog), "dialog-host-bound")) return;
  g_object_set_data (G_OBJECT (dialog), "dialog-host-bound", GINT_TO_POINTER (1));
  /* AdwDialog limits event propagation to its own subtree. Window-level
   * handlers never see real modal pointer events, even though direct signal
   * emission in a unit test appeared to work. Keep the modal barrier and
   * attach our capture controllers INSIDE it instead. */
  bind_controllers (GTK_WIDGET (dialog), window);
}

void
spotifygtk_dialog_host_bind (AdwApplicationWindow *window)
{
  g_return_if_fail (ADW_IS_APPLICATION_WINDOW (window));
  if (g_object_get_data (G_OBJECT (window), "dialog-host-bound")) return;
  g_object_set_data (G_OBJECT (window), "dialog-host-bound", GINT_TO_POINTER (1));
  bind_controllers (GTK_WIDGET (window), window);
  g_signal_connect (window, "notify::visible-dialog", G_CALLBACK (on_visible_dialog), NULL);
  on_visible_dialog (window, NULL, NULL);
}

#include "presentation_view.h"
#include "presentation_policy.h"
#include "settings.h"
#include "widget_layout.h"

/* A layout transform, not a second renderer or GtkViewport. GTK must snapshot
 * dirty children on every native frame; withholding those snapshots puts its
 * animation/damage bookkeeping out of sync with the displayed UI. Never keep
 * whole-window render nodes or drive painting with a GLib timer here. */
typedef struct {
  GtkWidget parent_instance;
  GtkWidget *child;
  GtkWidget *horizontal_bar, *vertical_bar; /* overlay-owned */
  GtkAdjustment *horizontal, *vertical;
  double scale;
  gboolean configuring;
  GtkRoot *focus_root; /* borrowed between root/unroot */
  gboolean trace;
  guint allocations, snapshots;
  gint64 allocation_us, snapshot_us, report_us;
  int logical_width, logical_height;
} SpotifyGtkPresentationView;

typedef GtkWidgetClass SpotifyGtkPresentationViewClass;
G_DEFINE_TYPE (SpotifyGtkPresentationView, spotifygtk_presentation_view, GTK_TYPE_WIDGET)

static void
presentation_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SpotifyGtkPresentationView *self = (SpotifyGtkPresentationView *) widget;
  gint64 started = self->trace ? g_get_monotonic_time () : 0;
  if (self->child) gtk_widget_snapshot_child (widget, self->child, snapshot);
  if (self->trace) {
    gint64 now = g_get_monotonic_time ();
    self->snapshots++;
    self->snapshot_us += now - started;
    if (now - self->report_us >= G_USEC_PER_SEC) {
      g_message ("presentation-profile: scale=%.2f viewport=%dx%d logical=%dx%d"
        " allocations=%u allocate-ms=%.2f snapshots=%u snapshot-ms=%.2f",
        self->scale, gtk_widget_get_width (widget), gtk_widget_get_height (widget),
        self->logical_width, self->logical_height, self->allocations,
        self->allocation_us / 1000.0, self->snapshots, self->snapshot_us / 1000.0);
      self->report_us = now;
      self->allocations = self->snapshots = 0;
      self->allocation_us = self->snapshot_us = 0;
    }
  }
}

static GtkSizeRequestMode
presentation_request_mode (GtkWidget *widget)
{
  SpotifyGtkPresentationView *self = (SpotifyGtkPresentationView *) widget;
  return self->child ? gtk_widget_get_request_mode (self->child) : GTK_SIZE_REQUEST_CONSTANT_SIZE;
}

static void
presentation_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                      int *minimum, int *natural, int *min_baseline, int *nat_baseline)
{
  SpotifyGtkPresentationView *self = (SpotifyGtkPresentationView *) widget;
  int min = 0, nat = 0;
  if (self->child)
    spotifygtk_widget_measure_fit (self->child, orientation,
      for_size < 0 ? -1 : (int) floor (for_size / self->scale), &min, &nat, NULL, NULL);
  /* Expose overflow without an outer GtkViewport negotiating natural sizes
   * against the virtualized page scrollers. */
  *minimum = 0;
  *natural = spotifygtk_presentation_extent (nat, self->scale);
  *min_baseline = *nat_baseline = -1;
}

static void
pan_changed (GtkAdjustment *adjustment, SpotifyGtkPresentationView *self)
{
  if (!self->configuring) gtk_widget_queue_allocate (GTK_WIDGET (self));
  (void) adjustment;
}

static void
configure_axis (GtkAdjustment *adjustment, int content, int page)
{
  double upper = MAX (content, page);
  double value = CLAMP (gtk_adjustment_get_value (adjustment), 0, upper - page);
  if (gtk_adjustment_get_upper (adjustment) == upper &&
      gtk_adjustment_get_page_size (adjustment) == page) return;
  gtk_adjustment_configure (adjustment, value, 0, upper, 32, MAX (1, page * 0.9), page);
}

static void
presentation_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  SpotifyGtkPresentationView *self = (SpotifyGtkPresentationView *) widget;
  if (!self->child) return;
  gint64 started = self->trace ? g_get_monotonic_time () : 0;
  int minimum_width = 0, minimum_height = 0;
  gtk_widget_measure (self->child, GTK_ORIENTATION_HORIZONTAL, -1,
                      &minimum_width, NULL, NULL, NULL);
  int logical_width = spotifygtk_presentation_logical_extent (width, self->scale, minimum_width);
  spotifygtk_widget_measure_fit (self->child, GTK_ORIENTATION_VERTICAL, logical_width,
                      &minimum_height, NULL, NULL, NULL);
  int logical_height = spotifygtk_presentation_logical_extent (height, self->scale, minimum_height);

  self->configuring = TRUE;
  int content_width = spotifygtk_presentation_extent (logical_width, self->scale);
  int content_height = spotifygtk_presentation_extent (logical_height, self->scale);
  configure_axis (self->horizontal, content_width, width);
  configure_axis (self->vertical, content_height, height);
  /* Overlay bars never take space from the content. Showing one cannot change
   * wrapping or row counts and start a scrollbar/layout feedback loop. */
  gtk_widget_set_visible (self->horizontal_bar, content_width > width);
  gtk_widget_set_visible (self->vertical_bar, content_height > height);
  self->configuring = FALSE;

  double x = gtk_adjustment_get_value (self->horizontal);
  double y = gtk_adjustment_get_value (self->vertical);
  GskTransform *transform = NULL;
  if (x || y)
    transform = gsk_transform_translate (transform, &GRAPHENE_POINT_INIT (-x, -y));
  if (self->scale != 1.0)
    transform = gsk_transform_scale (transform, self->scale, self->scale);
  gtk_widget_allocate (self->child, logical_width, logical_height,
    baseline < 0 ? -1 : (int) floor (baseline / self->scale), transform);
  if (self->trace) {
    self->logical_width = logical_width;
    self->logical_height = logical_height;
    self->allocations++;
    self->allocation_us += g_get_monotonic_time () - started;
  }
}

static gboolean
pan_axis (GtkAdjustment *adjustment, double delta)
{
  if (!delta) return FALSE;
  double old = gtk_adjustment_get_value (adjustment);
  double maximum = MAX (0, gtk_adjustment_get_upper (adjustment) -
                           gtk_adjustment_get_page_size (adjustment));
  double value = CLAMP (old + delta, 0, maximum);
  if (old == value) return FALSE;
  gtk_adjustment_set_value (adjustment, value);
  return TRUE;
}

static gboolean
on_overflow_scroll (GtkEventControllerScroll *controller, double dx, double dy, gpointer data)
{
  SpotifyGtkPresentationView *self = data;
  /* Bubble only: normal page/shelf scrolling gets first refusal. When content
   * fits, this does nothing and owns no kinetic-scrolling animation. */
  double step = gtk_event_controller_scroll_get_unit (controller) == GDK_SCROLL_UNIT_WHEEL ? 80 : 1;
  gboolean horizontal = pan_axis (self->horizontal, dx * step);
  gboolean vertical = pan_axis (self->vertical, dy * step);
  return horizontal || vertical;
}

static void
focus_changed (GtkRoot *root, GParamSpec *pspec, SpotifyGtkPresentationView *self)
{
  GtkWidget *focus = gtk_root_get_focus (root);
  graphene_rect_t bounds;
  if (!focus || !gtk_widget_is_ancestor (focus, GTK_WIDGET (self)) ||
      !gtk_widget_compute_bounds (focus, GTK_WIDGET (self), &bounds)) return;
  double right = bounds.origin.x + bounds.size.width;
  double bottom = bounds.origin.y + bounds.size.height;
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  int height = gtk_widget_get_height (GTK_WIDGET (self));
  pan_axis (self->horizontal, bounds.origin.x < 0 ? bounds.origin.x : MAX (0, right - width));
  pan_axis (self->vertical, bounds.origin.y < 0 ? bounds.origin.y : MAX (0, bottom - height));
  (void) pspec;
}

static void
presentation_root (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (spotifygtk_presentation_view_parent_class)->root (widget);
  SpotifyGtkPresentationView *self = (SpotifyGtkPresentationView *) widget;
  self->focus_root = gtk_widget_get_root (widget);
  if (GTK_IS_WINDOW (self->focus_root))
    g_signal_connect_object (self->focus_root, "notify::focus-widget", G_CALLBACK (focus_changed), self, 0);
}

static void
presentation_unroot (GtkWidget *widget)
{
  SpotifyGtkPresentationView *self = (SpotifyGtkPresentationView *) widget;
  if (self->focus_root) g_signal_handlers_disconnect_by_data (self->focus_root, self);
  self->focus_root = NULL;
  GTK_WIDGET_CLASS (spotifygtk_presentation_view_parent_class)->unroot (widget);
}

static void
presentation_changed (SpotifyGtkSettings *settings, SpotifyGtkPresentationView *self)
{
  double scale = spotifygtk_presentation_scale (spotifygtk_settings_get_ui_scale (settings));
  if (self->scale == scale) return;
  self->scale = scale;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
presentation_dispose (GObject *object)
{
  SpotifyGtkPresentationView *self = (SpotifyGtkPresentationView *) object;
  g_signal_handlers_disconnect_by_data (spotifygtk_settings_get_default (), self);
  if (self->focus_root) g_signal_handlers_disconnect_by_data (self->focus_root, self);
  self->focus_root = NULL;
  if (self->child) gtk_widget_unparent (g_steal_pointer (&self->child));
  if (self->horizontal) g_signal_handlers_disconnect_by_data (self->horizontal, self);
  if (self->vertical) g_signal_handlers_disconnect_by_data (self->vertical, self);
  g_clear_object (&self->horizontal);
  g_clear_object (&self->vertical);
  G_OBJECT_CLASS (spotifygtk_presentation_view_parent_class)->dispose (object);
}

static void
spotifygtk_presentation_view_class_init (SpotifyGtkPresentationViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = presentation_dispose;
  klass->get_request_mode = presentation_request_mode;
  klass->measure = presentation_measure;
  klass->size_allocate = presentation_allocate;
  klass->snapshot = presentation_snapshot;
  klass->root = presentation_root;
  klass->unroot = presentation_unroot;
  gtk_widget_class_set_css_name (klass, "presentation-view");
}

static void
spotifygtk_presentation_view_init (SpotifyGtkPresentationView *self)
{
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  self->trace = g_getenv ("SPOTIFY_PRESENTATION_TRACE") != NULL;
  self->scale = spotifygtk_presentation_scale (spotifygtk_settings_get_ui_scale (settings));
  self->horizontal = g_object_ref_sink (gtk_adjustment_new (0, 0, 0, 32, 100, 0));
  self->vertical = g_object_ref_sink (gtk_adjustment_new (0, 0, 0, 32, 100, 0));
  g_signal_connect_object (self->horizontal, "value-changed", G_CALLBACK (pan_changed), self, 0);
  g_signal_connect_object (self->vertical, "value-changed", G_CALLBACK (pan_changed), self, 0);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  g_signal_connect_object (settings, "changed", G_CALLBACK (presentation_changed), self, 0);
  GtkEventController *scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  gtk_event_controller_set_propagation_phase (scroll, GTK_PHASE_BUBBLE);
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_overflow_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll);
}

GtkWidget *
spotifygtk_presentation_wrap (GtkWidget *child)
{
  g_return_val_if_fail (GTK_IS_WIDGET (child), NULL);
  /* Default and explicit 100% use the original widget hierarchy exactly. */
  if (spotifygtk_presentation_scale (spotifygtk_settings_get_ui_scale (
        spotifygtk_settings_get_default ())) == 1.0) return child;
  SpotifyGtkPresentationView *view = g_object_new (spotifygtk_presentation_view_get_type (), NULL);
  view->child = child;
  gtk_widget_set_parent (child, GTK_WIDGET (view));
  GtkWidget *overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), GTK_WIDGET (view));
  view->horizontal_bar = gtk_scrollbar_new (GTK_ORIENTATION_HORIZONTAL, view->horizontal);
  view->vertical_bar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, view->vertical);
  gtk_widget_set_valign (view->horizontal_bar, GTK_ALIGN_END);
  gtk_widget_set_margin_end (view->horizontal_bar, 16);
  gtk_widget_set_halign (view->vertical_bar, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (view->vertical_bar, 16);
  gtk_widget_set_visible (view->horizontal_bar, FALSE);
  gtk_widget_set_visible (view->vertical_bar, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), view->horizontal_bar);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), view->vertical_bar);
  gtk_widget_set_hexpand (overlay, TRUE);
  gtk_widget_set_vexpand (overlay, TRUE);
  return overlay;
}

void
spotifygtk_presentation_detach (GtkWidget *wrapper)
{
  g_return_if_fail (GTK_IS_OVERLAY (wrapper));
  GtkWidget *widget = gtk_overlay_get_child (GTK_OVERLAY (wrapper));
  g_return_if_fail (G_TYPE_CHECK_INSTANCE_TYPE (widget, spotifygtk_presentation_view_get_type ()));
  SpotifyGtkPresentationView *self = (SpotifyGtkPresentationView *) widget;
  if (self->child) gtk_widget_unparent (g_steal_pointer (&self->child));
}

static void
dialog_scale_changed (SpotifyGtkSettings *settings, AdwDialog *dialog)
{
  double scale = spotifygtk_presentation_scale (spotifygtk_settings_get_ui_scale (settings));
  int width = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dialog), "presentation-width"));
  int height = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dialog), "presentation-height"));
  if (width > 0) adw_dialog_set_content_width (dialog, spotifygtk_presentation_extent (width, scale));
  if (height > 0) adw_dialog_set_content_height (dialog, spotifygtk_presentation_extent (height, scale));
}

void
spotifygtk_presentation_wrap_dialog (AdwDialog *dialog)
{
  if (gtk_widget_has_css_class (GTK_WIDGET (dialog), "artwork-viewer")) return;
  if (spotifygtk_presentation_scale (spotifygtk_settings_get_ui_scale (
        spotifygtk_settings_get_default ())) == 1.0) return;
  GtkWidget *child = adw_dialog_get_child (dialog);
  if (!child) return;
  GtkWidget *focus = adw_dialog_get_focus (dialog);
  if (focus) g_object_ref (focus);
  g_object_ref (child);
  adw_dialog_set_child (dialog, NULL);
  GtkWidget *overlay = spotifygtk_presentation_wrap (child);
  g_object_unref (child);
  adw_dialog_set_child (dialog, overlay);
  if (focus) {
    adw_dialog_set_focus (dialog, focus);
    g_object_unref (focus);
  }
  g_object_set_data (G_OBJECT (dialog), "presentation-width",
    GINT_TO_POINTER (adw_dialog_get_content_width (dialog)));
  g_object_set_data (G_OBJECT (dialog), "presentation-height",
    GINT_TO_POINTER (adw_dialog_get_content_height (dialog)));
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  dialog_scale_changed (settings, dialog);
  g_signal_connect_object (settings, "changed", G_CALLBACK (dialog_scale_changed), dialog, 0);
}

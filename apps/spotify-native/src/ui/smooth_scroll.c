/*
 * smooth_scroll.c — see smooth_scroll.h.
 */

#include "smooth_scroll.h"
#include "settings.h"
#include <math.h>

#define SMOOTH_SCROLL_FRAME_US 16666.0  /* what that fraction is calibrated against */
/* A long stall should catch up, not teleport: past this the easing saturates. */
#define SMOOTH_SCROLL_MAX_FRAMES 6.0
/* List/grid adjustments may re-anchor to integral row pixels after every
 * write. Easing asymptotically toward a fractional target can consequently
 * hover a handful of pixels away forever. Eight pixels is below one tenth of
 * a wheel step and visually indistinguishable from the exact landing point. */
#define SMOOTH_SCROLL_STOP_EPSILON 8.0
#define SMOOTH_SCROLL_STALL_FRAMES 3
/* GtkListView can make small anchor corrections as recycled rows are measured.
 * A larger change is a new page/layout or direct scroll takeover, not a row
 * rounding correction. Treating up to half a wheel notch as an anchor let an
 * old wheel target chase page-load adjustments and visibly oscillate. */
#define SMOOTH_SCROLL_ANCHOR_TOLERANCE 12.0

typedef struct {
  GtkScrolledWindow *scroller;      /* borrowed; owns this via set_data */
  GWeakRef           nested_horizontal;
  gboolean           nested_hover;
  GtkOrientation     orientation;
  guint              tick;
  gdouble            target;
  gdouble            last_set;      /* what we last wrote, to spot outside changes */
  gint64             last_frame_us; /* to ease by elapsed time, not by frame count */
  gboolean           writing_adjustment;
  gboolean           trace;
  gdouble            observed_value;
  gint64             observed_us;
  gint               observed_direction;
  gdouble            progress_value;
  guint              stalled_frames;
  GtkAdjustment      *observed_adjustment; /* owned while handlers are live */
  gulong              value_handler;
  gulong              upper_handler;
  gulong              page_handler;
  guint64            gesture;
  guint              events;
  guint              frames;
  gint64             started_us;
  gint64             max_frame_gap_us;
  guint              frame_gaps_over_33ms;
  guint              frame_gaps_over_100ms;
  guint              adjustments;
  guint              external_adjustments;
  guint              reversals;
  guint              geometry_changes;
  guint              anchor_corrections;
  gdouble            max_adjustment_delta;
} SmoothScroll;

/* The centre reproduces the tuned 118px/0.30 behaviour. Moving toward Glide
 * increases the accumulated travel (wheel gravity) while lowering the easing
 * fraction, so motion carries farther and sheds momentum more gradually. */
static void
scroll_character (gdouble *step, gdouble *ease)
{
  gdouble amount = spotifygtk_settings_get_scroll_smoothness (
    spotifygtk_settings_get_default ()) / 100.0;
  if (step)
    *step = 88.0 + amount * 60.0;
  if (ease)
    *ease = 0.44 - amount * 0.28;
}

static void
smooth_scroll_free (gpointer data)
{
  SmoothScroll *ss = data;
  g_weak_ref_clear (&ss->nested_horizontal);
  if (ss->observed_adjustment) {
    if (ss->value_handler)
      g_signal_handler_disconnect (ss->observed_adjustment, ss->value_handler);
    if (ss->upper_handler)
      g_signal_handler_disconnect (ss->observed_adjustment, ss->upper_handler);
    if (ss->page_handler)
      g_signal_handler_disconnect (ss->observed_adjustment, ss->page_handler);
    g_clear_object (&ss->observed_adjustment);
  }
  g_free (ss);
}

static void
set_adjustment_value (SmoothScroll *ss, GtkAdjustment *adj, gdouble value)
{
  /* GtkListBase stores adjustment positions as integers, then configures
   * the adjustment from that integer anchor during allocation. Fractional
   * wheel writes therefore generate a second value-changed every frame.
   * Match its coordinate precision before notifying the view. Other
   * scrollable widgets retain their subpixel motion. */
  GtkWidget *child = gtk_scrolled_window_get_child (ss->scroller);
  if (GTK_IS_LIST_VIEW (child) || GTK_IS_GRID_VIEW (child))
    value = round (value);
  ss->writing_adjustment = TRUE;
  gtk_adjustment_set_value (adj, value);
  ss->writing_adjustment = FALSE;
}

static void
on_adjustment_value (GtkAdjustment *adj, gpointer user_data)
{
  SmoothScroll *ss = user_data;
  if (!ss->trace)
    return;

  gint64 now = g_get_monotonic_time ();
  gdouble value = gtk_adjustment_get_value (adj);
  gdouble delta = value - ss->observed_value;
  gint direction = delta > 0.5 ? 1 : delta < -0.5 ? -1 : 0;
  gboolean reversal = direction != 0 && ss->observed_direction != 0 &&
    direction != ss->observed_direction && now - ss->observed_us < 150000;

  ss->adjustments++;
  if (!ss->writing_adjustment)
    ss->external_adjustments++;
  if (reversal)
    ss->reversals++;
  ss->max_adjustment_delta = MAX (ss->max_adjustment_delta, fabs (delta));

  if (direction != 0)
    ss->observed_direction = direction;
  ss->observed_value = value;
  ss->observed_us = now;
}

static void
on_adjustment_geometry (GtkAdjustment *adj, GParamSpec *pspec, gpointer user_data)
{
  SmoothScroll *ss = user_data;
  if (ss->trace)
    ss->geometry_changes++;
  (void) adj;
  (void) pspec;
}

static const gchar *
orientation_name (GtkOrientation orientation)
{
  return orientation == GTK_ORIENTATION_HORIZONTAL ? "horizontal" : "vertical";
}

static void
log_scroll_end (SmoothScroll *ss, const gchar *reason, gdouble value)
{
  if (!ss->trace)
    return;
  gint64 elapsed = ss->started_us > 0
    ? g_get_monotonic_time () - ss->started_us : 0;
  g_message ("scroll-profile: gesture=%" G_GUINT64_FORMAT
             " axis=%s end=%s events=%u frames=%u elapsed=%.1fms"
             " max-frame-gap=%.1fms gaps>33ms=%u gaps>100ms=%u"
             " adjustments=%u external=%u reversals=%u geometry=%u"
             " anchors=%u max-adjustment=%.1fpx value=%.1f target=%.1f",
             ss->gesture, orientation_name (ss->orientation), reason,
             ss->events, ss->frames, elapsed / 1000.0,
             ss->max_frame_gap_us / 1000.0, ss->frame_gaps_over_33ms,
             ss->frame_gaps_over_100ms, ss->adjustments,
             ss->external_adjustments, ss->reversals, ss->geometry_changes,
             ss->anchor_corrections, ss->max_adjustment_delta, value, ss->target);
}

static GtkAdjustment *
adjustment_for (SmoothScroll *ss)
{
  return ss->orientation == GTK_ORIENTATION_HORIZONTAL
    ? gtk_scrolled_window_get_hadjustment (ss->scroller)
    : gtk_scrolled_window_get_vadjustment (ss->scroller);
}

/* An inline list or horizontal shelf may be inside another page scroller.
 * Only pass an outward edge event on if that ancestor can actually use it;
 * otherwise GTK's own controller starts overshoot on the exhausted scroller.
 * Repeated edge input can then leave its unclamped position at the limit and
 * make the next inward gesture appear stuck. */
static gboolean
ancestor_can_scroll (SmoothScroll *ss, GtkOrientation axis, gdouble delta)
{
  for (GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (ss->scroller));
       parent; parent = gtk_widget_get_parent (parent)) {
    if (!GTK_IS_SCROLLED_WINDOW (parent))
      continue;
    GtkAdjustment *adj = axis == GTK_ORIENTATION_HORIZONTAL
      ? gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (parent))
      : gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (parent));
    if (!adj)
      continue;
    gdouble lower = gtk_adjustment_get_lower (adj);
    gdouble upper = MAX (lower, gtk_adjustment_get_upper (adj) -
                               gtk_adjustment_get_page_size (adj));
    gdouble value = gtk_adjustment_get_value (adj);
    if ((delta < 0 && value > lower) || (delta > 0 && value < upper))
      return TRUE;
  }
  return FALSE;
}

static GtkScrolledWindow *
registered_horizontal_shelf_at_event (SmoothScroll *ss,
                                      GtkEventControllerScroll *ctrl)
{
  if (ss->orientation != GTK_ORIENTATION_VERTICAL)
    return NULL;
  /* Search registers its shelf explicitly. Route input from the parent capture
   * controller using the shelf's allocated rectangle, regardless of which
   * GtkListView section-header child GTK picked as the event target. */
  GtkScrolledWindow *nested = g_weak_ref_get (&ss->nested_horizontal);
  if (!nested)
    return NULL;
  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (nested);
  if (!hadj || !gtk_widget_get_mapped (GTK_WIDGET (nested)) ||
      gtk_adjustment_get_upper (hadj) <= gtk_adjustment_get_page_size (hadj)) {
    g_object_unref (nested);
    return NULL;
  }

  /* Pointer enter/leave comes from GTK's actual hit testing, so it remains
   * reliable when a section header intercepts the scroll event itself. */
  if (ss->nested_hover)
    return nested;

  GdkEvent *event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (ctrl));
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (ss->scroller));
  gdouble x, y, tx, ty;
  if (!event || !native || !gdk_event_get_position (event, &x, &y)) {
    g_object_unref (nested);
    return NULL;
  }
  gtk_native_get_surface_transform (native, &tx, &ty);
  graphene_rect_t bounds;
  gboolean inside = gtk_widget_compute_bounds (GTK_WIDGET (nested),
                                                GTK_WIDGET (native), &bounds) &&
    x + tx >= bounds.origin.x && x + tx < bounds.origin.x + bounds.size.width &&
    y + ty >= bounds.origin.y && y + ty < bounds.origin.y + bounds.size.height;
  if (inside)
    return nested;
  g_object_unref (nested);

  return NULL;
}

static void
on_nested_enter (GtkEventControllerMotion *motion, gdouble x, gdouble y,
                 gpointer user_data)
{
  GtkScrolledWindow *parent = user_data;
  SmoothScroll *ss = g_object_get_data (G_OBJECT (parent),
                                        "spotifygtk-smooth-scroll");
  if (ss)
    ss->nested_hover = TRUE;
  (void) motion;
  (void) x;
  (void) y;
}

static void
on_nested_leave (GtkEventControllerMotion *motion, gpointer user_data)
{
  GtkScrolledWindow *parent = user_data;
  SmoothScroll *ss = g_object_get_data (G_OBJECT (parent),
                                        "spotifygtk-smooth-scroll");
  if (ss)
    ss->nested_hover = FALSE;
  (void) motion;
}

static gboolean
event_over_nested_horizontal_shelf (SmoothScroll *ss,
                                    GtkEventControllerScroll *ctrl)
{
  if (ss->orientation != GTK_ORIENTATION_VERTICAL)
    return FALSE;
  GdkEvent *event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (ctrl));
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (ss->scroller));
  gdouble x, y, tx, ty;
  if (!event || !native || !gdk_event_get_position (event, &x, &y))
    return FALSE;
  gtk_native_get_surface_transform (native, &tx, &ty);

  GtkWidget *picked = gtk_widget_pick (GTK_WIDGET (native), x + tx, y + ty,
                                      GTK_PICK_DEFAULT);
  for (GtkWidget *w = picked; w && w != GTK_WIDGET (ss->scroller);
       w = gtk_widget_get_parent (w)) {
    if (GTK_IS_SCROLLED_WINDOW (w)) {
      GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (
        GTK_SCROLLED_WINDOW (w));
      if (hadj && gtk_adjustment_get_upper (hadj) >
                  gtk_adjustment_get_page_size (hadj))
        return TRUE;
    }
  }
  return FALSE;
}

static gboolean
smooth_scroll_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  SmoothScroll  *ss  = user_data;
  GtkAdjustment *adj = adjustment_for (ss);
  (void) widget;

  if (!adj) {
    log_scroll_end (ss, "no-adjustment", 0.0);
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }

  gdouble value = gtk_adjustment_get_value (adj);

  /* GtkListView performs small viewport-anchor corrections when its recycled
   * rows are remeasured. Preserve the remaining animation distance across
   * those corrections; otherwise every correction looks like outside input,
   * kills the easing after a few frames, and the next correction can visibly
   * pull the viewport back. A large displacement is a real scrollbar,
   * keyboard or programmatic takeover and still wins immediately. */
  gdouble outside_delta = value - ss->last_set;
  if (ABS (outside_delta) > 1.0 &&
      ABS (outside_delta) <= SMOOTH_SCROLL_ANCHOR_TOLERANCE) {
    ss->target += outside_delta;
    ss->last_set = value;
    if (ss->trace)
      ss->anchor_corrections++;
  } else if (ABS (outside_delta) > SMOOTH_SCROLL_ANCHOR_TOLERANCE) {
    log_scroll_end (ss, "external-change", value);
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }

  /*
   * Re-clamp against the bounds as they are now, not as they were when the
   * wheel turned.
   *
   * These lists grow and shrink under the pointer -- rows spliced in, a grid
   * replaced, a page rebuilt -- and the target was clamped once, at wheel
   * time. When the content shrank afterwards the animation went on driving
   * toward a position past the end, and GTK clamped every step, so the view
   * slid to the bottom and stayed. The same in reverse put it at the top.
   * That is the viewport "randomly" jumping to one end.
   */
  gdouble lower = gtk_adjustment_get_lower (adj);
  gdouble upper = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
  if (upper < lower)
    upper = lower;

  gdouble clamped = CLAMP (ss->target, lower, upper);
  if (clamped != ss->target) {
    /* The content moved out from under the gesture. Finish where the list can
     * actually go and stop, rather than animating into a wall. */
    ss->target = clamped;
    set_adjustment_value (ss, adj, clamped);
    log_scroll_end (ss, "bounds-changed", clamped);
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }

  gdouble remaining = ss->target - value;
  /* GtkAdjustment and the virtualized view can quantise/re-anchor separately.
   * Do not wait for an exact floating-point target that the view cannot hold. */
  if (ABS (remaining) <= SMOOTH_SCROLL_STOP_EPSILON) {
    set_adjustment_value (ss, adj, ss->target);
    log_scroll_end (ss, "complete", ss->target);
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }

  /*
   * Ease by elapsed time, not by frame.
   *
   * A fixed fraction per frame is only a fixed speed if the frames arrive on
   * time. This app drops around one frame in six even when idle -- measured,
   * steady, roughly ten a second, with occasional half-second hitches while a
   * large list loads -- so a per-frame ease produced half as many steps at
   * twice the size exactly when the machine was busiest. That is mechanically
   * the same motion as GtkScrolledWindow's stepped wheel handling, which is
   * why scrolling "went back to default GTK" while media was loading and
   * recovered afterwards. It was never switching handlers.
   *
   * Raising the per-frame fraction to the elapsed number of 60Hz frames keeps
   * the travel time constant however the frames actually land, so jank costs
   * smoothness rather than changing the character of the motion.
  */
  gint64  now_us  = gdk_frame_clock_get_frame_time (clock);
  gint64  frame_gap_us = ss->trace && ss->last_frame_us > 0
    ? now_us - ss->last_frame_us : 0;
  gdouble frames  = (ss->last_frame_us > 0)
    ? (gdouble) (now_us - ss->last_frame_us) / SMOOTH_SCROLL_FRAME_US : 1.0;
  frames = CLAMP (frames, 0.1, SMOOTH_SCROLL_MAX_FRAMES);
  ss->last_frame_us = now_us;

  if (ss->trace) {
    ss->frames++;
    ss->max_frame_gap_us = MAX (ss->max_frame_gap_us, frame_gap_us);
    if (frame_gap_us > 33000)
      ss->frame_gaps_over_33ms++;
    if (frame_gap_us > 100000)
      ss->frame_gaps_over_100ms++;
  }

  gdouble ease = 0.30;
  scroll_character (NULL, &ease);
  gdouble factor = 1.0 - pow (1.0 - ease, frames);

  set_adjustment_value (ss, adj, value + remaining * factor);
  ss->last_set = gtk_adjustment_get_value (adj);

  /* A view can accept our fractional value and snap it back before the next
   * tick, evading the immediate equality check below. Detect lack of progress
   * across ticks as well and finish the gesture deterministically. */
  if (ABS (value - ss->progress_value) < 0.5)
    ss->stalled_frames++;
  else
    ss->stalled_frames = 0;
  ss->progress_value = value;

  if (ss->stalled_frames >= SMOOTH_SCROLL_STALL_FRAMES) {
    set_adjustment_value (ss, adj, ss->target);
    ss->last_set = gtk_adjustment_get_value (adj);
    log_scroll_end (ss, "stalled", ss->last_set);
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }

  /* Be defensive about other quantisation steps too. If GTK accepted no
   * movement at all, another eased frame cannot improve the situation; snap
   * once rather than leaving a permanent frame-clock callback behind. */
  if (ss->last_set == value) {
    set_adjustment_value (ss, adj, ss->target);
    ss->last_set = gtk_adjustment_get_value (adj);
    log_scroll_end (ss, "quantized", ss->last_set);
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean
on_scroll (GtkEventControllerScroll *ctrl, gdouble dx, gdouble dy, gpointer user_data)
{
  SmoothScroll *ss = user_data;

  GtkScrolledWindow *nested = registered_horizontal_shelf_at_event (ss, ctrl);
  gboolean nested_at_edge = nested != NULL;
  if (nested) {
    SmoothScroll *child = g_object_get_data (G_OBJECT (nested),
                                             "spotifygtk-smooth-scroll");
    gboolean handled = child && on_scroll (ctrl, dx, dy, child);
    g_object_unref (nested);
    if (handled)
      return GDK_EVENT_STOP;
    /* The shelf reached an edge; the parent can take this gesture. */
  }

  /* Capture runs from the parent toward the leaf. Give an embedded album
   * shelf first refusal before the page's wheel animation claims the event. */
  if (!nested_at_edge && event_over_nested_horizontal_shelf (ss, ctrl))
    return GDK_EVENT_PROPAGATE;

  GtkAdjustment *adj = adjustment_for (ss);
  gdouble delta = ss->orientation == GTK_ORIENTATION_HORIZONTAL && dx != 0.0
    ? dx : dy;
  if (!adj || delta == 0.0)
    return GDK_EVENT_PROPAGATE;

  gdouble lower = gtk_adjustment_get_lower (adj);
  gdouble upper = MAX (lower, gtk_adjustment_get_upper (adj) -
                             gtk_adjustment_get_page_size (adj));
  gdouble value = gtk_adjustment_get_value (adj);

#if GTK_CHECK_VERSION (4, 8, 0)
  /* GTK owns touchpad gestures, including their overshoot/end sequence. If
   * we stop only the edge event, GTK can be left with a live kinetic gesture
   * that pins the adjustment even when the user drags the scrollbar back. */
  if (gtk_event_controller_scroll_get_unit (ctrl) != GDK_SCROLL_UNIT_WHEEL) {
    if (ss->tick != 0) {
      log_scroll_end (ss, "touchpad-takeover", value);
      gtk_widget_remove_tick_callback (GTK_WIDGET (ss->scroller), ss->tick);
      ss->tick = 0;
    }
    if (ss->orientation == GTK_ORIENTATION_HORIZONTAL) {
      gdouble motion = fabs (dx) > fabs (dy) ? dx : dy;
      if ((motion < 0 && value > lower) || (motion > 0 && value < upper)) {
        set_adjustment_value (ss, adj, CLAMP (value + motion, lower, upper));
        return GDK_EVENT_STOP;
      }
    }
    return GDK_EVENT_PROPAGATE;
  }
#else
  (void) ctrl;
#endif

  if ((delta < 0 && value <= lower) || (delta > 0 && value >= upper)) {
    if (ss->tick) {
      log_scroll_end (ss, "edge", value);
      gtk_widget_remove_tick_callback (GTK_WIDGET (ss->scroller), ss->tick);
      ss->tick = 0;
      ss->target = ss->last_set = value;
    }
    /* A wheel over a horizontal shelf can continue vertically on its parent.
     * A terminal page edge, however, should not enter GTK overshoot at all. */
    GtkOrientation parent_axis = ss->orientation == GTK_ORIENTATION_HORIZONTAL &&
                                 dy != 0.0 ? GTK_ORIENTATION_VERTICAL :
                                             ss->orientation;
    return ancestor_can_scroll (ss, parent_axis, delta)
      ? GDK_EVENT_PROPAGATE : GDK_EVENT_STOP;
  }

  /* A wheel only ever reports dy, so a horizontal target has to take it from
   * there; dx is preferred when present (tilt wheels, horizontal gestures). */
  /* Accumulate onto an in-flight target so a flick of several notches travels
   * the whole distance instead of each notch cancelling the last. */
  gdouble base = (ss->tick != 0) ? ss->target : value;

  if (ss->trace && ss->tick == 0) {
    ss->gesture++;
    ss->events = 0;
    ss->frames = 0;
    ss->started_us = g_get_monotonic_time ();
    ss->max_frame_gap_us = 0;
    ss->frame_gaps_over_33ms = 0;
    ss->frame_gaps_over_100ms = 0;
    ss->adjustments = 0;
    ss->external_adjustments = 0;
    ss->reversals = 0;
    ss->geometry_changes = 0;
    ss->anchor_corrections = 0;
    ss->max_adjustment_delta = 0.0;
  }
  if (ss->trace)
    ss->events++;
  if (ss->tick == 0) {
    ss->progress_value = value;
    ss->stalled_frames = 0;
  }

  gdouble step = 118.0;
  scroll_character (&step, NULL);
  ss->target   = CLAMP (base + delta * step, lower, upper);
  ss->last_set = value;
  if (ss->tick == 0) {
    ss->last_frame_us = 0;   /* first frame of a new flick eases by one frame */
    ss->tick = gtk_widget_add_tick_callback (GTK_WIDGET (ss->scroller),
                                             smooth_scroll_tick, ss, NULL);
  }
  return GDK_EVENT_STOP;
}

void
spotifygtk_smooth_scroll_attach (GtkScrolledWindow *scroller,
                                 GtkOrientation     orientation)
{
  g_return_if_fail (GTK_IS_SCROLLED_WINDOW (scroller));

  SmoothScroll *ss = g_new0 (SmoothScroll, 1);
  ss->scroller    = scroller;
  ss->orientation = orientation;
  ss->trace       = g_getenv ("SPOTIFY_SCROLL_STATS") != NULL;
  g_weak_ref_init (&ss->nested_horizontal, NULL);

  /* Tied to the widget's lifetime: the tick callback is removed with the
   * widget, and this frees with it, so there is nothing to unregister. */
  g_object_set_data_full (G_OBJECT (scroller), "spotifygtk-smooth-scroll",
                          ss, smooth_scroll_free);

  GtkAdjustment *adj = adjustment_for (ss);
  if (adj) {
    ss->observed_adjustment = g_object_ref (adj);
    ss->observed_value = gtk_adjustment_get_value (adj);
    ss->value_handler = g_signal_connect (adj, "value-changed",
      G_CALLBACK (on_adjustment_value), ss);
    ss->upper_handler = g_signal_connect (adj, "notify::upper",
      G_CALLBACK (on_adjustment_geometry), ss);
    ss->page_handler = g_signal_connect (adj, "notify::page-size",
      G_CALLBACK (on_adjustment_geometry), ss);
  }

  /* BOTH_AXES so a horizontal target still sees the vertical wheel.
   * DISCRETE asks GTK to normalise physical wheel detents for this controller
   * while continuous touchpad events pass through to GtkScrolledWindow.  On
   * high-resolution libinput wheels, BOTH_AXES alone can expose the event as a
   * surface delta; the unit guard above then correctly leaves it to GTK, but
   * the visible result is the old stepped scrolling instead of this easing. */
  GtkEventController *wheel =
    gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES |
                                     GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);

  /*
   * CAPTURE, so this runs *before* GtkScrolledWindow's own scroll handling
   * rather than after it. In the default bubble phase both handlers acted on
   * the same event: the scrolled window applied its stepped jump first, which
   * moved the adjustment behind this animation's back, and the tick below
   * correctly concluded that something else had taken over and bailed out.
   * The visible result was scrolling that intermittently reverted to the old
   * stepped feel for a stretch and then went smooth again.
   */
  gtk_event_controller_set_propagation_phase (wheel, GTK_PHASE_CAPTURE);
  g_signal_connect (wheel, "scroll", G_CALLBACK (on_scroll), ss);
  gtk_widget_add_controller (GTK_WIDGET (scroller), wheel);
}

void
spotifygtk_smooth_scroll_set_nested_horizontal (GtkScrolledWindow *parent,
                                                GtkScrolledWindow *child)
{
  g_return_if_fail (GTK_IS_SCROLLED_WINDOW (parent));
  g_return_if_fail (GTK_IS_SCROLLED_WINDOW (child));
  SmoothScroll *ss = g_object_get_data (G_OBJECT (parent),
                                        "spotifygtk-smooth-scroll");
  g_return_if_fail (ss != NULL);
  g_weak_ref_set (&ss->nested_horizontal, child);
  ss->nested_hover = FALSE;

  GtkEventController *motion = gtk_event_controller_motion_new ();
  gtk_event_controller_set_propagation_phase (motion, GTK_PHASE_CAPTURE);
  g_signal_connect_object (motion, "enter", G_CALLBACK (on_nested_enter),
                           parent, 0);
  g_signal_connect_object (motion, "leave", G_CALLBACK (on_nested_leave),
                           parent, 0);
  gtk_widget_add_controller (GTK_WIDGET (child), motion);
}

gboolean
spotifygtk_smooth_scroll_get_target (GtkScrolledWindow *scroller,
                                     gdouble           *target)
{
  g_return_val_if_fail (GTK_IS_SCROLLED_WINDOW (scroller), FALSE);

  SmoothScroll *ss = g_object_get_data (G_OBJECT (scroller),
                                        "spotifygtk-smooth-scroll");
  if (!ss || ss->tick == 0)
    return FALSE;

  if (target)
    *target = ss->target;
  return TRUE;
}

void
spotifygtk_smooth_scroll_cancel (GtkScrolledWindow *scroller)
{
  g_return_if_fail (GTK_IS_SCROLLED_WINDOW (scroller));
  SmoothScroll *ss = g_object_get_data (G_OBJECT (scroller),
                                        "spotifygtk-smooth-scroll");
  if (!ss || !ss->tick)
    return;
  GtkAdjustment *adj = adjustment_for (ss);
  gdouble value = adj ? gtk_adjustment_get_value (adj) : 0.0;
  log_scroll_end (ss, "navigation", value);
  gtk_widget_remove_tick_callback (GTK_WIDGET (scroller), ss->tick);
  ss->tick = 0;
  ss->target = ss->last_set = value;
}

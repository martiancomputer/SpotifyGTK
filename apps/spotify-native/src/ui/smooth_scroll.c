/*
 * smooth_scroll.c — see smooth_scroll.h.
 */

#include "smooth_scroll.h"
#include "settings.h"
#include "../log_verbose.h"

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
/* GtkListView adjusts its value by a few pixels while recycled rows above the
 * viewport are remeasured.  That is viewport anchoring, not another input
 * device taking ownership.  A wheel notch is 118px, so corrections below half
 * a notch are safely distinguishable from a scrollbar drag/default wheel. */
#define SMOOTH_SCROLL_ANCHOR_TOLERANCE 59.0

typedef struct {
  GtkScrolledWindow *scroller;      /* borrowed; owns this via set_data */
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
#ifdef SPOTIFYGTK_VERBOSE
  guint64            gesture;
  guint              events;
  guint              frames;
  gint64             started_us;
  gint64             max_frame_gap_us;
#endif
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

  g_message ("scroll-trace: adjustment source=%s value=%.2f delta=%+.2f "
             "target=%.2f active=%s%s",
             ss->writing_adjustment ? "smooth" : "external", value, delta,
             ss->target, ss->tick ? "yes" : "no",
             reversal ? " REVERSAL" : "");

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
    g_message ("scroll-trace: geometry changed=%s lower=%.2f upper=%.2f "
               "page=%.2f value=%.2f target=%.2f active=%s",
               pspec->name, gtk_adjustment_get_lower (adj),
               gtk_adjustment_get_upper (adj),
               gtk_adjustment_get_page_size (adj),
               gtk_adjustment_get_value (adj), ss->target,
               ss->tick ? "yes" : "no");
}

#ifdef SPOTIFYGTK_VERBOSE
static const gchar *
orientation_name (GtkOrientation orientation)
{
  return orientation == GTK_ORIENTATION_HORIZONTAL ? "horizontal" : "vertical";
}

static void
log_scroll_end (SmoothScroll *ss, const gchar *reason, gdouble value)
{
  gint64 elapsed = ss->started_us > 0
    ? g_get_monotonic_time () - ss->started_us : 0;
  SPOTIFYGTK_DEBUG ("wheel: gesture=%" G_GUINT64_FORMAT
                    " axis=%s end=%s events=%u frames=%u elapsed=%.1fms"
                    " max-frame-gap=%.1fms value=%.2f target=%.2f",
                    ss->gesture, orientation_name (ss->orientation), reason,
                    ss->events, ss->frames, elapsed / 1000.0,
                    ss->max_frame_gap_us / 1000.0, value, ss->target);
}
#endif

static GtkAdjustment *
adjustment_for (SmoothScroll *ss)
{
  return ss->orientation == GTK_ORIENTATION_HORIZONTAL
    ? gtk_scrolled_window_get_hadjustment (ss->scroller)
    : gtk_scrolled_window_get_vadjustment (ss->scroller);
}

static gboolean
smooth_scroll_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  SmoothScroll  *ss  = user_data;
  GtkAdjustment *adj = adjustment_for (ss);
  (void) widget;

  if (!adj) {
#ifdef SPOTIFYGTK_VERBOSE
    log_scroll_end (ss, "no-adjustment", 0.0);
#endif
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
      g_message ("scroll-trace: absorbed anchor delta=%+.2f target=%.2f",
                 outside_delta, ss->target);
  } else if (ABS (outside_delta) > SMOOTH_SCROLL_ANCHOR_TOLERANCE) {
#ifdef SPOTIFYGTK_VERBOSE
    log_scroll_end (ss, "external-change", value);
#endif
    if (ss->trace)
      g_message ("scroll-trace: ownership takeover delta=%+.2f", outside_delta);
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
#ifdef SPOTIFYGTK_VERBOSE
    log_scroll_end (ss, "bounds-changed", clamped);
#endif
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }

  gdouble remaining = ss->target - value;
  /* GtkAdjustment and the virtualized view can quantise/re-anchor separately.
   * Do not wait for an exact floating-point target that the view cannot hold. */
  if (ABS (remaining) <= SMOOTH_SCROLL_STOP_EPSILON) {
    set_adjustment_value (ss, adj, ss->target);
#ifdef SPOTIFYGTK_VERBOSE
    log_scroll_end (ss, "complete", ss->target);
#endif
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
  gint64  frame_gap_us = ss->last_frame_us > 0 ? now_us - ss->last_frame_us : 0;
  gdouble frames  = (ss->last_frame_us > 0)
    ? (gdouble) (now_us - ss->last_frame_us) / SMOOTH_SCROLL_FRAME_US : 1.0;
  frames = CLAMP (frames, 0.1, SMOOTH_SCROLL_MAX_FRAMES);
  ss->last_frame_us = now_us;

#ifdef SPOTIFYGTK_VERBOSE
  ss->frames++;
  if (frame_gap_us > ss->max_frame_gap_us)
    ss->max_frame_gap_us = frame_gap_us;
  /* 33-67ms gaps are already visible in the aggregate end record. Logging
   * every such frame performs synchronous terminal/file work in the GTK loop
   * and makes an overloaded gesture worse. Only record a true long stall. */
  if (frame_gap_us > 100000)
    SPOTIFYGTK_DEBUG ("wheel: gesture=%" G_GUINT64_FORMAT
                      " frame-gap=%.1fms frame=%u value=%.2f target=%.2f remaining=%.2f",
                      ss->gesture, frame_gap_us / 1000.0, ss->frames,
                      value, ss->target, remaining);
#endif

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
#ifdef SPOTIFYGTK_VERBOSE
    log_scroll_end (ss, "stalled", ss->last_set);
#endif
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }

  /* Be defensive about other quantisation steps too. If GTK accepted no
   * movement at all, another eased frame cannot improve the situation; snap
   * once rather than leaving a permanent frame-clock callback behind. */
  if (ss->last_set == value) {
    set_adjustment_value (ss, adj, ss->target);
    ss->last_set = gtk_adjustment_get_value (adj);
#ifdef SPOTIFYGTK_VERBOSE
    log_scroll_end (ss, "quantized", ss->last_set);
#endif
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean
on_scroll (GtkEventControllerScroll *ctrl, gdouble dx, gdouble dy, gpointer user_data)
{
  SmoothScroll *ss = user_data;

#if GTK_CHECK_VERSION (4, 8, 0)
  /* Touchpads already deliver continuous pixel deltas and GTK handles those
   * well, including kinetic follow-through. Only the discrete case needs help. */
  if (gtk_event_controller_scroll_get_unit (ctrl) != GDK_SCROLL_UNIT_WHEEL) {
    /* A touchpad gesture owns the adjustment from this event onward.  Cancel
     * an unfinished wheel animation immediately so its next frame cannot tug
     * against GTK's kinetic motion and produce a one-frame reversal. */
    if (ss->tick != 0) {
      if (ss->trace)
        g_message ("scroll-trace: touchpad takes ownership; cancel target=%.2f",
                   ss->target);
      gtk_widget_remove_tick_callback (GTK_WIDGET (ss->scroller), ss->tick);
      ss->tick = 0;
    }
    return GDK_EVENT_PROPAGATE;
  }
#else
  (void) ctrl;
#endif

  /* A wheel only ever reports dy, so a horizontal target has to take it from
   * there; dx is preferred when present (tilt wheels, horizontal gestures). */
  gdouble delta = (ss->orientation == GTK_ORIENTATION_HORIZONTAL && dx != 0.0) ? dx : dy;
  if (ss->trace)
    g_message ("scroll-trace: input unit=%d dx=%+.3f dy=%+.3f chosen=%+.3f "
               "active=%s",
               (gint) gtk_event_controller_scroll_get_unit (ctrl), dx, dy,
               delta, ss->tick ? "yes" : "no");
  if (delta == 0.0)
    return GDK_EVENT_PROPAGATE;

  GtkAdjustment *adj = adjustment_for (ss);
  if (!adj)
    return GDK_EVENT_PROPAGATE;

  gdouble lower = gtk_adjustment_get_lower (adj);
  gdouble upper = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
  if (upper < lower)
    upper = lower;

  gdouble value = gtk_adjustment_get_value (adj);

  /* Already hard against the end in the direction asked for: propagate, so an
   * enclosing scroller can take over instead of the event dying here. */
  if ((delta < 0 && value <= lower) || (delta > 0 && value >= upper))
    return GDK_EVENT_PROPAGATE;

  /* Accumulate onto an in-flight target so a flick of several notches travels
   * the whole distance instead of each notch cancelling the last. */
  gdouble base = (ss->tick != 0) ? ss->target : value;

#ifdef SPOTIFYGTK_VERBOSE
  if (ss->tick == 0) {
    ss->gesture++;
    ss->events = 0;
    ss->frames = 0;
    ss->started_us = g_get_monotonic_time ();
    ss->max_frame_gap_us = 0;
    ss->progress_value = value;
    ss->stalled_frames = 0;
  }
  ss->events++;
#endif

  gdouble step = 118.0;
  scroll_character (&step, NULL);
  ss->target   = CLAMP (base + delta * step, lower, upper);
  ss->last_set = value;
#ifdef SPOTIFYGTK_VERBOSE
  SPOTIFYGTK_DEBUG ("wheel: gesture=%" G_GUINT64_FORMAT
                    " event=%u axis=%s unit=%d dx=%.3f dy=%.3f delta=%.3f"
                    " value=%.2f base=%.2f target=%.2f bounds=%.2f..%.2f",
                    ss->gesture, ss->events, orientation_name (ss->orientation),
                    (gint) gtk_event_controller_scroll_get_unit (ctrl),
                    dx, dy, delta, value, base, ss->target, lower, upper);
#endif
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

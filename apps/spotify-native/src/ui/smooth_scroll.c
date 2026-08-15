/*
 * smooth_scroll.c — see smooth_scroll.h.
 */

#include "smooth_scroll.h"

#include <math.h>

#define SMOOTH_SCROLL_STEP  118.0   /* px travelled per wheel notch */
#define SMOOTH_SCROLL_EASE  0.24    /* fraction of the remaining gap, per 60Hz frame */
#define SMOOTH_SCROLL_FRAME_US 16666.0  /* what that fraction is calibrated against */
/* A long stall should catch up, not teleport: past this the easing saturates. */
#define SMOOTH_SCROLL_MAX_FRAMES 6.0

typedef struct {
  GtkScrolledWindow *scroller;      /* borrowed; owns this via set_data */
  GtkOrientation     orientation;
  guint              tick;
  gdouble            target;
  gdouble            last_set;      /* what we last wrote, to spot outside changes */
  gint64             last_frame_us; /* to ease by elapsed time, not by frame count */
} SmoothScroll;

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
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }

  gdouble value = gtk_adjustment_get_value (adj);

  /* Someone grabbed the scrollbar, or a keyboard or programmatic scroll landed,
   * while this was still animating. Their position wins -- drop the animation
   * rather than dragging the view back out from under them. */
  if (ABS (value - ss->last_set) > 1.0) {
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
    gtk_adjustment_set_value (adj, clamped);
    ss->tick = 0;
    return G_SOURCE_REMOVE;
  }

  gdouble remaining = ss->target - value;
  if (ABS (remaining) < 0.5) {
    gtk_adjustment_set_value (adj, ss->target);
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
  gdouble frames  = (ss->last_frame_us > 0)
    ? (gdouble) (now_us - ss->last_frame_us) / SMOOTH_SCROLL_FRAME_US : 1.0;
  frames = CLAMP (frames, 0.1, SMOOTH_SCROLL_MAX_FRAMES);
  ss->last_frame_us = now_us;

  gdouble factor = 1.0 - pow (1.0 - SMOOTH_SCROLL_EASE, frames);

  gtk_adjustment_set_value (adj, value + remaining * factor);
  ss->last_set = gtk_adjustment_get_value (adj);
  return G_SOURCE_CONTINUE;
}

static gboolean
on_scroll (GtkEventControllerScroll *ctrl, gdouble dx, gdouble dy, gpointer user_data)
{
  SmoothScroll *ss = user_data;

#if GTK_CHECK_VERSION (4, 8, 0)
  /* Touchpads already deliver continuous pixel deltas and GTK handles those
   * well, including kinetic follow-through. Only the discrete case needs help. */
  if (gtk_event_controller_scroll_get_unit (ctrl) != GDK_SCROLL_UNIT_WHEEL)
    return GDK_EVENT_PROPAGATE;
#else
  (void) ctrl;
#endif

  /* A wheel only ever reports dy, so a horizontal target has to take it from
   * there; dx is preferred when present (tilt wheels, horizontal gestures). */
  gdouble delta = (ss->orientation == GTK_ORIENTATION_HORIZONTAL && dx != 0.0) ? dx : dy;
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

  ss->target   = CLAMP (base + delta * SMOOTH_SCROLL_STEP, lower, upper);
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

  /* Tied to the widget's lifetime: the tick callback is removed with the
   * widget, and this frees with it, so there is nothing to unregister. */
  g_object_set_data_full (G_OBJECT (scroller), "spotifygtk-smooth-scroll",
                          ss, g_free);

  /* BOTH_AXES so a horizontal target still sees the vertical wheel. */
  GtkEventController *wheel =
    gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);

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

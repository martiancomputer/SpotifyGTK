/*
 * eq_graph.c — Draggable equaliser response curve.
 *
 * The curve is not an interpolation through the handles: it is the real summed
 * magnitude response of the biquad cascade, from the same coefficient maths the
 * audio path runs (spotifygtk_eq_response_curve). What is drawn is what is
 * heard — including the parts a slider strip hides, like two adjacent bands
 * summing to more boost than either was set to.
 *
 * PERFORMANCE
 *
 * The first version called a per-frequency magnitude helper once per pixel
 * column, and that helper recomputed all fifteen biquads from scratch every
 * time — roughly 16,000 pow/cos/log10 calls per frame, on the main thread,
 * during a drag. It was visibly laggy. Now the coefficients are computed once
 * per curve, and the curve itself is cached: it is rebuilt only when a gain
 * actually changes or the widget is resized, not on every frame.
 *
 * Axes: log frequency across (20 Hz .. 20 kHz, matching how pitch is heard),
 * linear dB up (+-12, the range the settings clamp to).
 */

#include "eq_graph.h"
#include "../audio/dsp.h"

#include <math.h>

#define EQ_MIN_DB     (-12.0)
#define EQ_MAX_DB     ( 12.0)
#define EQ_MIN_HZ     20.0
#define EQ_MAX_HZ     20000.0
#define GRAPH_HEIGHT  210
#define PAD_L         42.0
#define PAD_R         14.0
#define PAD_T         14.0
#define PAD_B         26.0
#define HANDLE_R      4.5

/* The filter shape is rate-dependent; this is the rate Spotify decodes at. */
#define DISPLAY_RATE  44100

struct _SpotifyGtkEqGraph {
  GtkDrawingArea parent_instance;

  gdouble gains[SPOTIFYGTK_EQ_BANDS];
  gint    active_band;      /* -1 when not dragging */
  gint    hover_band;       /* -1 when the pointer is away */
  gdouble drag_start_y;

  /* Cached response, one dB value per pixel column of the plot area. */
  gdouble *curve;
  gint     curve_len;
  gboolean curve_dirty;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkEqGraph, spotifygtk_eq_graph, GTK_TYPE_DRAWING_AREA)

enum { BAND_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* --- coordinate mapping --- */

static gdouble
freq_to_x (gdouble hz, gdouble w)
{
  gdouble t = (log10 (hz) - log10 (EQ_MIN_HZ)) /
              (log10 (EQ_MAX_HZ) - log10 (EQ_MIN_HZ));
  return PAD_L + t * (w - PAD_L - PAD_R);
}

static gdouble
x_to_freq (gdouble x, gdouble w)
{
  gdouble t = (x - PAD_L) / MAX (1.0, w - PAD_L - PAD_R);
  return pow (10.0, log10 (EQ_MIN_HZ) + t * (log10 (EQ_MAX_HZ) - log10 (EQ_MIN_HZ)));
}

static gdouble
db_to_y (gdouble db, gdouble h)
{
  gdouble t = (EQ_MAX_DB - db) / (EQ_MAX_DB - EQ_MIN_DB);
  return PAD_T + t * (h - PAD_T - PAD_B);
}

static gdouble
y_to_db (gdouble y, gdouble h)
{
  gdouble t = (y - PAD_T) / MAX (1.0, h - PAD_T - PAD_B);
  return CLAMP (EQ_MAX_DB - t * (EQ_MAX_DB - EQ_MIN_DB), EQ_MIN_DB, EQ_MAX_DB);
}

static gchar *
freq_label (gint hz)
{
  if (hz >= 1000)
    return g_strdup_printf ("%gk", hz / 1000.0);
  return g_strdup_printf ("%d", hz);
}

/* --- cached curve --- */

/* Rebuilt only when a gain changes or the width does; see the header comment
 * on why this is not done per frame. */
static void
rebuild_curve (SpotifyGtkEqGraph *self, gdouble w)
{
  gint n = (gint) (w - PAD_L - PAD_R);
  if (n < 2)
    return;

  if (n != self->curve_len) {
    g_free (self->curve);
    self->curve     = g_new0 (gdouble, n);
    self->curve_len = n;
  }

  g_autofree gdouble *freqs = g_new (gdouble, n);
  for (gint i = 0; i < n; i++)
    freqs[i] = x_to_freq (PAD_L + i, w);

  spotifygtk_eq_response_curve (self->gains, DISPLAY_RATE, freqs, self->curve, n);
  self->curve_dirty = FALSE;
}

/* --- drawing --- */

static void
draw_func (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  SpotifyGtkEqGraph *self = SPOTIFYGTK_EQ_GRAPH (area);
  gdouble w = width, h = height;
  gdouble plot_top = PAD_T, plot_bot = h - PAD_B;
  gdouble x0 = PAD_L, x1 = w - PAD_R;

  if (x1 - x0 < 4.0 || plot_bot - plot_top < 4.0)
    return;

  if (self->curve_dirty || self->curve_len != (gint) (x1 - x0))
    rebuild_curve (self, w);
  if (!self->curve)
    return;

  GdkRGBA fg;
  gtk_widget_get_color (GTK_WIDGET (area), &fg);
  const gdouble ar = 0.114, ag = 0.725, ab = 0.329;   /* @accent #1db954 */

  cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, 10.0);
  cairo_set_line_width (cr, 1.0);

  /* Frequency guides: decades bright, the labelled bands faint. */
  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b += 2) {
    gdouble x = freq_to_x (spotifygtk_eq_frequencies[b], w);
    cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.07);
    cairo_move_to (cr, x, plot_top);
    cairo_line_to (cr, x, plot_bot);
    cairo_stroke (cr);

    g_autofree gchar *lbl = freq_label (spotifygtk_eq_frequencies[b]);
    cairo_text_extents_t ext;
    cairo_text_extents (cr, lbl, &ext);
    cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.45);
    cairo_move_to (cr, x - ext.width / 2.0, h - 8.0);
    cairo_show_text (cr, lbl);
  }

  /* dB gridlines, 0 dB emphasised, labels right-aligned in the gutter. */
  for (gdouble db = EQ_MIN_DB; db <= EQ_MAX_DB + 0.1; db += 6.0) {
    gdouble y = db_to_y (db, h);
    gboolean zero = fabs (db) < 0.001;
    cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, zero ? 0.28 : 0.10);
    cairo_move_to (cr, x0, y);
    cairo_line_to (cr, x1, y);
    cairo_stroke (cr);

    g_autofree gchar *lbl = g_strdup_printf ("%+g", db);
    cairo_text_extents_t ext;
    cairo_text_extents (cr, lbl, &ext);
    cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.45);
    cairo_move_to (cr, PAD_L - 8.0 - ext.width, y + 3.5);
    cairo_show_text (cr, lbl);
  }

  /* Clip so the fill and curve cannot bleed into the label gutters. */
  cairo_save (cr);
  cairo_rectangle (cr, x0, plot_top, x1 - x0, plot_bot - plot_top);
  cairo_clip (cr);

  /* Trace the response once; reuse the path for the fill and the stroke. */
  cairo_new_path (cr);
  for (gint i = 0; i < self->curve_len; i++) {
    gdouble x = x0 + i;
    gdouble y = db_to_y (CLAMP (self->curve[i], EQ_MIN_DB, EQ_MAX_DB), h);
    if (i == 0) cairo_move_to (cr, x, y);
    else        cairo_line_to (cr, x, y);
  }
  cairo_path_t *curve = cairo_copy_path (cr);

  /* Fill from the curve down to the floor, fading out — the shape reads as a
   * solid mass hanging off the curve rather than a band pinned to 0 dB, which
   * is what made the old fill-to-zero look like a detached blob. */
  cairo_line_to (cr, x1, plot_bot);
  cairo_line_to (cr, x0, plot_bot);
  cairo_close_path (cr);

  cairo_pattern_t *grad = cairo_pattern_create_linear (0, plot_top, 0, plot_bot);
  cairo_pattern_add_color_stop_rgba (grad, 0.0, ar, ag, ab, 0.34);
  cairo_pattern_add_color_stop_rgba (grad, 1.0, ar, ag, ab, 0.02);
  cairo_set_source (cr, grad);
  cairo_fill (cr);
  cairo_pattern_destroy (grad);

  cairo_new_path (cr);
  cairo_append_path (cr, curve);
  cairo_path_destroy (curve);
  cairo_set_source_rgba (cr, ar, ag, ab, 0.95);
  cairo_set_line_width (cr, 2.0);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke (cr);

  cairo_restore (cr);

  /* Handles sit on the curve at each band centre. */
  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++) {
    gdouble x = freq_to_x (spotifygtk_eq_frequencies[b], w);
    gint    i = CLAMP ((gint) (x - x0), 0, self->curve_len - 1);
    gdouble y = db_to_y (CLAMP (self->curve[i], EQ_MIN_DB, EQ_MAX_DB), h);
    gboolean hot = (b == self->active_band || b == self->hover_band);
    gdouble  r   = hot ? HANDLE_R + 1.5 : HANDLE_R;

    cairo_set_source_rgba (cr, ar, ag, ab, 1.0);
    cairo_arc (cr, x, y, r, 0, 2 * G_PI);
    cairo_fill (cr);

    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, hot ? 0.95 : 0.7);
    cairo_set_line_width (cr, 1.5);
    cairo_arc (cr, x, y, r, 0, 2 * G_PI);
    cairo_stroke (cr);
  }
  (void) data;
}

/* --- interaction --- */

/* Nearest band by horizontal distance: the handle rides the curve, so its
 * height moves as neighbours change, and x is what the pointer is aiming at. */
static gint
band_at_x (gdouble x, gdouble w)
{
  gint    best = 0;
  gdouble best_d = G_MAXDOUBLE;
  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++) {
    gdouble d = fabs (freq_to_x (spotifygtk_eq_frequencies[b], w) - x);
    if (d < best_d) { best_d = d; best = b; }
  }
  return best;
}

static void
apply_drag (SpotifyGtkEqGraph *self, gdouble y)
{
  if (self->active_band < 0)
    return;

  gdouble h  = gtk_widget_get_height (GTK_WIDGET (self));
  /* Snap to whole dB: the settings store works in 1 dB steps, and a continuous
   * value would write a setting and rebuild the curve on every motion event. */
  gdouble db = CLAMP (round (y_to_db (y, h)), EQ_MIN_DB, EQ_MAX_DB);

  if (self->gains[self->active_band] == db)
    return;

  self->gains[self->active_band] = db;
  self->curve_dirty = TRUE;
  g_signal_emit (self, signals[BAND_CHANGED], 0, (guint) self->active_band, db);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_begin (GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer user_data)
{
  SpotifyGtkEqGraph *self = user_data;
  self->active_band  = band_at_x (x, gtk_widget_get_width (GTK_WIDGET (self)));
  self->drag_start_y = y;
  apply_drag (self, y);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  (void) gesture;
}

static void
on_drag_update (GtkGestureDrag *gesture, gdouble ox, gdouble oy, gpointer user_data)
{
  SpotifyGtkEqGraph *self = user_data;
  apply_drag (self, self->drag_start_y + oy);
  (void) gesture; (void) ox;
}

static void
on_drag_end (GtkGestureDrag *gesture, gdouble ox, gdouble oy, gpointer user_data)
{
  SpotifyGtkEqGraph *self = user_data;
  apply_drag (self, self->drag_start_y + oy);
  self->active_band = -1;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  (void) gesture; (void) ox;
}

static void
on_motion (GtkEventControllerMotion *ctl, gdouble x, gdouble y, gpointer user_data)
{
  SpotifyGtkEqGraph *self = user_data;
  gint b = band_at_x (x, gtk_widget_get_width (GTK_WIDGET (self)));
  if (b != self->hover_band) {
    self->hover_band = b;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
  (void) ctl; (void) y;
}

static void
on_motion_leave (GtkEventControllerMotion *ctl, gpointer user_data)
{
  SpotifyGtkEqGraph *self = user_data;
  if (self->hover_band != -1) {
    self->hover_band = -1;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
  (void) ctl;
}

/* --- boilerplate --- */

void
spotifygtk_eq_graph_set_gains (SpotifyGtkEqGraph *self, const gdouble *gains_db)
{
  g_return_if_fail (SPOTIFYGTK_IS_EQ_GRAPH (self));
  if (!gains_db)
    return;
  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++)
    self->gains[b] = CLAMP (gains_db[b], EQ_MIN_DB, EQ_MAX_DB);
  self->curve_dirty = TRUE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
spotifygtk_eq_graph_finalize (GObject *object)
{
  SpotifyGtkEqGraph *self = SPOTIFYGTK_EQ_GRAPH (object);
  g_clear_pointer (&self->curve, g_free);
  G_OBJECT_CLASS (spotifygtk_eq_graph_parent_class)->finalize (object);
}

static void
spotifygtk_eq_graph_class_init (SpotifyGtkEqGraphClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = spotifygtk_eq_graph_finalize;

  signals[BAND_CHANGED] = g_signal_new (
    "band-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
    NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_DOUBLE);
}

static void
spotifygtk_eq_graph_init (SpotifyGtkEqGraph *self)
{
  self->active_band = -1;
  self->hover_band  = -1;
  self->curve_dirty = TRUE;
  gtk_widget_set_size_request (GTK_WIDGET (self), -1, GRAPH_HEIGHT);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);

  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self), draw_func, NULL, NULL);

  GtkGesture *drag = gtk_gesture_drag_new ();
  g_signal_connect (drag, "drag-begin",  G_CALLBACK (on_drag_begin),  self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end",    G_CALLBACK (on_drag_end),    self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
  g_signal_connect (motion, "leave",  G_CALLBACK (on_motion_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);
}

SpotifyGtkEqGraph *
spotifygtk_eq_graph_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_EQ_GRAPH, NULL);
}

/*
 * eq_graph.c — Draggable equaliser response curve.
 *
 * The curve is not an interpolation through the handles: every pixel column is
 * a real evaluation of the cascade's magnitude response at that frequency
 * (spotifygtk_eq_magnitude_db, the same coefficient maths the audio path runs).
 * That means what is drawn is what is heard — including the parts a slider
 * strip hides, like two adjacent bands summing to more boost than either was
 * set to, or a lone band's skirts leaking an octave either side.
 *
 * Axes: log frequency across (20 Hz .. 20 kHz, matching how pitch is heard),
 * linear dB up (+-12, the same range the settings clamp to).
 */

#include "eq_graph.h"
#include "../audio/dsp.h"

#include <math.h>

#define EQ_MIN_DB     (-12.0)
#define EQ_MAX_DB     ( 12.0)
#define EQ_MIN_HZ     20.0
#define EQ_MAX_HZ     20000.0
#define GRAPH_HEIGHT  190
#define PAD_L         38.0    /* room for the dB scale */
#define PAD_R         10.0
#define PAD_T         10.0
#define PAD_B         24.0    /* room for the frequency labels */
#define HANDLE_R      5.5

/* The response is drawn at the rate Spotify actually decodes at; the filter
 * shape is rate-dependent and this is the one that matters in practice. */
#define DISPLAY_RATE  44100

struct _SpotifyGtkEqGraph {
  GtkDrawingArea parent_instance;

  gdouble gains[SPOTIFYGTK_EQ_BANDS];
  gint    active_band;    /* -1 when not dragging */
  gdouble drag_start_y;
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
db_to_y (gdouble db, gdouble h)
{
  gdouble t = (EQ_MAX_DB - db) / (EQ_MAX_DB - EQ_MIN_DB);
  return PAD_T + t * (h - PAD_T - PAD_B);
}

static gdouble
y_to_db (gdouble y, gdouble h)
{
  gdouble t = (y - PAD_T) / (h - PAD_T - PAD_B);
  return CLAMP (EQ_MAX_DB - t * (EQ_MAX_DB - EQ_MIN_DB), EQ_MIN_DB, EQ_MAX_DB);
}

static gchar *
freq_label (gint hz)
{
  if (hz >= 1000)
    return g_strdup_printf ("%gk", hz / 1000.0);
  return g_strdup_printf ("%d", hz);
}

/* --- drawing --- */

static void
draw_func (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  SpotifyGtkEqGraph *self = SPOTIFYGTK_EQ_GRAPH (area);
  gdouble w = width, h = height;

  /* Foreground from the theme so the grid and labels follow light/dark; the
   * curve uses the app accent, which is the same in every palette. */
  GdkRGBA fg;
  gtk_widget_get_color (GTK_WIDGET (area), &fg);
  const gdouble ar = 0.114, ag = 0.725, ab = 0.329;   /* @accent #1db954 */

  cairo_set_line_width (cr, 1.0);
  cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, 10.0);

  /* Horizontal dB gridlines, 0 dB emphasised. */
  for (gdouble db = EQ_MIN_DB; db <= EQ_MAX_DB + 0.1; db += 6.0) {
    gdouble y = db_to_y (db, h);
    gboolean zero = fabs (db) < 0.001;
    cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, zero ? 0.34 : 0.13);
    cairo_move_to (cr, PAD_L, y);
    cairo_line_to (cr, w - PAD_R, y);
    cairo_stroke (cr);

    g_autofree gchar *lbl = g_strdup_printf ("%+g", db);
    cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.5);
    cairo_move_to (cr, 6.0, y + 3.5);
    cairo_show_text (cr, lbl);
  }

  /* Vertical decade guides. */
  static const gint guides[] = { 100, 1000, 10000 };
  for (gsize i = 0; i < G_N_ELEMENTS (guides); i++) {
    gdouble x = freq_to_x (guides[i], w);
    cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.13);
    cairo_move_to (cr, x, PAD_T);
    cairo_line_to (cr, x, h - PAD_B);
    cairo_stroke (cr);
  }

  /* The response curve, evaluated per pixel column. */
  gdouble x0 = PAD_L, x1 = w - PAD_R;
  cairo_new_path (cr);
  for (gdouble x = x0; x <= x1; x += 1.0) {
    gdouble t  = (x - x0) / MAX (1.0, x1 - x0);
    gdouble hz = pow (10.0, log10 (EQ_MIN_HZ) +
                            t * (log10 (EQ_MAX_HZ) - log10 (EQ_MIN_HZ)));
    gdouble db = spotifygtk_eq_magnitude_db (self->gains, hz, DISPLAY_RATE);
    gdouble y  = db_to_y (CLAMP (db, EQ_MIN_DB, EQ_MAX_DB), h);
    if (x == x0) cairo_move_to (cr, x, y);
    else         cairo_line_to (cr, x, y);
  }

  /* Keep the stroke, reuse the path to shade the area back to 0 dB. */
  cairo_path_t *curve = cairo_copy_path (cr);
  cairo_line_to (cr, x1, db_to_y (0.0, h));
  cairo_line_to (cr, x0, db_to_y (0.0, h));
  cairo_close_path (cr);
  cairo_set_source_rgba (cr, ar, ag, ab, 0.16);
  cairo_fill (cr);

  cairo_new_path (cr);
  cairo_append_path (cr, curve);
  cairo_path_destroy (curve);
  cairo_set_source_rgba (cr, ar, ag, ab, 0.95);
  cairo_set_line_width (cr, 2.0);
  cairo_stroke (cr);

  /* Handles, one per band, on the curve at that band's centre. */
  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++) {
    gdouble hz = spotifygtk_eq_frequencies[b];
    gdouble x  = freq_to_x (hz, w);
    gdouble db = spotifygtk_eq_magnitude_db (self->gains, hz, DISPLAY_RATE);
    gdouble y  = db_to_y (CLAMP (db, EQ_MIN_DB, EQ_MAX_DB), h);
    gboolean hot = (b == self->active_band);

    cairo_set_source_rgba (cr, ar, ag, ab, 1.0);
    cairo_arc (cr, x, y, hot ? HANDLE_R + 1.5 : HANDLE_R, 0, 2 * G_PI);
    cairo_fill (cr);

    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, hot ? 1.0 : 0.85);
    cairo_set_line_width (cr, 1.6);
    cairo_arc (cr, x, y, hot ? HANDLE_R + 1.5 : HANDLE_R, 0, 2 * G_PI);
    cairo_stroke (cr);

    /* Label every other band so the axis does not crowd at 15 bands. */
    if (b % 2 == 0) {
      g_autofree gchar *lbl = freq_label (spotifygtk_eq_frequencies[b]);
      cairo_text_extents_t ext;
      cairo_text_extents (cr, lbl, &ext);
      cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.5);
      cairo_move_to (cr, x - ext.width / 2.0, h - 3.0);
      cairo_show_text (cr, lbl);
    }
  }
  (void) data;
}

/* --- interaction --- */

/* Nearest band by horizontal distance: the handle sits on the curve, so its
 * height moves as neighbours change, and picking by x alone is both simpler
 * and what the pointer is actually aiming at. */
static gint
band_at_x (SpotifyGtkEqGraph *self, gdouble x, gdouble w)
{
  gint    best = 0;
  gdouble best_d = G_MAXDOUBLE;
  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++) {
    gdouble d = fabs (freq_to_x (spotifygtk_eq_frequencies[b], w) - x);
    if (d < best_d) { best_d = d; best = b; }
  }
  (void) self;
  return best;
}

static void
apply_drag (SpotifyGtkEqGraph *self, gdouble y)
{
  if (self->active_band < 0)
    return;

  gdouble h  = gtk_widget_get_height (GTK_WIDGET (self));
  gdouble db = y_to_db (y, h);

  /* Snap to whole dB: the settings store and the slider strip before it both
   * worked in 1 dB steps, and a continuous value would write a new setting on
   * every motion event. */
  db = CLAMP (round (db), EQ_MIN_DB, EQ_MAX_DB);

  if (self->gains[self->active_band] == db)
    return;

  self->gains[self->active_band] = db;
  g_signal_emit (self, signals[BAND_CHANGED], 0,
                 (guint) self->active_band, db);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_begin (GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer user_data)
{
  SpotifyGtkEqGraph *self = user_data;
  self->active_band  = band_at_x (self, x, gtk_widget_get_width (GTK_WIDGET (self)));
  self->drag_start_y = y;
  apply_drag (self, y);
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

/* --- boilerplate --- */

void
spotifygtk_eq_graph_set_gains (SpotifyGtkEqGraph *self, const gdouble *gains_db)
{
  g_return_if_fail (SPOTIFYGTK_IS_EQ_GRAPH (self));
  if (!gains_db)
    return;
  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++)
    self->gains[b] = CLAMP (gains_db[b], EQ_MIN_DB, EQ_MAX_DB);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
spotifygtk_eq_graph_class_init (SpotifyGtkEqGraphClass *klass)
{
  signals[BAND_CHANGED] = g_signal_new (
    "band-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
    NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_DOUBLE);
}

static void
spotifygtk_eq_graph_init (SpotifyGtkEqGraph *self)
{
  self->active_band = -1;
  gtk_widget_set_size_request (GTK_WIDGET (self), -1, GRAPH_HEIGHT);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);

  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self), draw_func, NULL, NULL);

  GtkGesture *drag = gtk_gesture_drag_new ();
  g_signal_connect (drag, "drag-begin",  G_CALLBACK (on_drag_begin),  self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end",    G_CALLBACK (on_drag_end),    self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

SpotifyGtkEqGraph *
spotifygtk_eq_graph_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_EQ_GRAPH, NULL);
}

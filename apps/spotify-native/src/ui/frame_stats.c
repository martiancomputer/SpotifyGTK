#include "frame_stats.h"
#include <adwaita.h>
#include <math.h>

typedef struct {
  GtkWidget *window; /* owned by the window's data slot */
  GdkFrameClock *clock;
  gulong before_id, after_id;
  gint64 started_us, paint_started_us, last_frame_us;
  gint64 total_paint_us, max_paint_us, max_gap_us;
  guint frames, late_frames;
  gint width, height;
  gboolean trace_layout;
  gint last_minimum;
  struct { gint64 total_us, max_us; guint count; } work[SPOTIFYGTK_FRAME_WORK_COUNT];
} FrameStats;

/* Opt-in incident capture: record geometry, never catalogue text. */
static void
trace_minimum_tree (GtkWidget *widget, guint depth, guint *remaining)
{
  if (!*remaining || depth > 32 ||
      (depth > 0 && !gtk_widget_should_layout (widget))) return;
  (*remaining)--;
  gint minimum, natural, request_width, request_height;
  gtk_widget_measure (widget, GTK_ORIENTATION_HORIZONTAL, -1,
                      &minimum, &natural, NULL, NULL);
  gtk_widget_get_size_request (widget, &request_width, &request_height);
  g_auto(GStrv) classes = gtk_widget_get_css_classes (widget);
  g_autofree gchar *css = g_strjoinv (" ", classes);
  g_message ("layout-minimum: depth=%u type=%s css=%s min=%d natural=%d"
             " allocated=%dx%d request=%dx%d",
             depth, G_OBJECT_TYPE_NAME (widget), css, minimum, natural,
             gtk_widget_get_width (widget), gtk_widget_get_height (widget),
             request_width, request_height);
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    trace_minimum_tree (child, depth + 1, remaining);
}

gint64
spotifygtk_frame_stats_work_begin (void)
{
  static gsize enabled;
  if (g_once_init_enter (&enabled))
    g_once_init_leave (&enabled, g_getenv ("SPOTIFY_SCROLL_STATS") ? 2 : 1);
  return enabled == 2 ? g_get_monotonic_time () : 0;
}

void
spotifygtk_frame_stats_work_end (GtkWidget *widget,
                                 SpotifyGtkFrameWork work, gint64 started)
{
  if (!started || work >= SPOTIFYGTK_FRAME_WORK_COUNT)
    return;
  gint64 elapsed = g_get_monotonic_time () - started;
  GtkRoot *root = gtk_widget_get_root (widget);
  FrameStats *stats = root ? g_object_get_data (G_OBJECT (root), "frame-stats") : NULL;
  if (!stats)
    return;
  stats->work[work].total_us += elapsed;
  stats->work[work].max_us = MAX (stats->work[work].max_us, elapsed);
  stats->work[work].count++;
}

static void
frame_stats_free (gpointer data)
{
  FrameStats *stats = data;
  g_signal_handler_disconnect (stats->clock, stats->before_id);
  g_signal_handler_disconnect (stats->clock, stats->after_id);
  g_object_unref (stats->clock);
  g_free (stats);
}

static void
before_paint (GdkFrameClock *clock, gpointer data)
{
  FrameStats *stats = data;
  stats->paint_started_us = g_get_monotonic_time ();
  (void) clock;
}

static void
after_paint (GdkFrameClock *clock, gpointer data)
{
  FrameStats *stats = data;
  gint64 now = g_get_monotonic_time ();
  gint64 frame = gdk_frame_clock_get_frame_time (clock);
  gint64 refresh = 0;
  gdk_frame_clock_get_refresh_info (clock, frame, &refresh, NULL);
  gint width = gtk_widget_get_width (stats->window);
  gint height = gtk_widget_get_height (stats->window);
  if (stats->trace_layout) {
    GtkWidget *content = ADW_IS_APPLICATION_WINDOW (stats->window)
      ? adw_application_window_get_content (ADW_APPLICATION_WINDOW (stats->window))
      : gtk_window_get_child (GTK_WINDOW (stats->window));
    if (GTK_IS_OVERLAY (content)) {
      GtkWidget *view = gtk_overlay_get_child (GTK_OVERLAY (content));
      if (view && g_str_equal (gtk_widget_get_css_name (view), "presentation-view"))
        content = gtk_widget_get_first_child (view);
    }
    if (!content) content = stats->window;
    gint minimum;
    gtk_widget_measure (content, GTK_ORIENTATION_HORIZONTAL, -1,
                        &minimum, NULL, NULL, NULL);
    graphene_rect_t area;
    if (gtk_widget_get_width (content) > 0 &&
        gtk_widget_compute_bounds (content, stats->window, &area))
      minimum = (gint) MIN (G_MAXINT,
        ceil (minimum * (double) area.size.width / gtk_widget_get_width (content)));
    if (minimum > width && minimum != stats->last_minimum) {
      guint remaining = 300;
      trace_minimum_tree (stats->window, 0, &remaining);
    }
    stats->last_minimum = minimum > width ? minimum : 0;
  }
  if (width != stats->width || height != stats->height) {
    g_message ("frame-profile: resize %dx%d -> %dx%d", stats->width,
               stats->height, width, height);
    stats->width = width;
    stats->height = height;
    stats->last_frame_us = 0;
  }

  gint64 paint = MAX (now - stats->paint_started_us, 0);
  stats->total_paint_us += paint;
  stats->max_paint_us = MAX (stats->max_paint_us, paint);
  if (stats->last_frame_us) {
    gint64 gap = frame - stats->last_frame_us;
    /* Idle gaps are not missed frames. Only classify gaps during animation
     * or interaction, when the clock is producing frames within 250ms. */
    if (gap > 0 && gap < 250000) {
      stats->max_gap_us = MAX (stats->max_gap_us, gap);
      if (refresh > 0 && gap > refresh + refresh / 2)
        stats->late_frames++;
    }
  }
  stats->last_frame_us = frame;
  stats->frames++;
  if (now - stats->started_us >= 5000000) {
    GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (stats->window));
    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (stats->window));
    g_message ("frame-profile: renderer=%s size=%dx%d frames=%u"
               " paint-avg=%.2fms paint-max=%.2fms gap-max=%.2fms"
               " late=%u refresh=%.2fms surface=%dx%d scale=%.3f focused=%d"
               " limit=%u unsync-request=%u unsync-available=%u",
               renderer ? G_OBJECT_TYPE_NAME (renderer) : "unavailable",
               width, height, stats->frames,
               stats->total_paint_us / (1000.0 * stats->frames),
               stats->max_paint_us / 1000.0, stats->max_gap_us / 1000.0,
               stats->late_frames, refresh / 1000.0,
               surface ? gdk_surface_get_width (surface) : 0,
               surface ? gdk_surface_get_height (surface) : 0,
               surface ? gdk_surface_get_scale (surface) : 0,
               gtk_window_is_active (GTK_WINDOW (stats->window)),
               GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (clock), "gtk4-enhanced-max-fps-v1")),
               GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (clock), "gtk4-enhanced-unsynchronized-v1")),
               GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (clock), "gtk4-enhanced-unsynchronized-available-v1")));
    static const gchar *names[] = { "row-bind", "header-bind", "glass-snapshot" };
    for (guint i = 0; i < SPOTIFYGTK_FRAME_WORK_COUNT; i++) {
      if (stats->work[i].count)
        g_message ("frame-work-profile: stage=%s calls=%u total=%.2fms max=%.2fms",
          names[i], stats->work[i].count, stats->work[i].total_us / 1000.0,
          stats->work[i].max_us / 1000.0);
      stats->work[i].total_us = stats->work[i].max_us = 0;
      stats->work[i].count = 0;
    }
    stats->started_us = now;
    stats->frames = stats->late_frames = 0;
    stats->total_paint_us = stats->max_paint_us = stats->max_gap_us = 0;
  }
}

void
spotifygtk_frame_stats_attach (GtkWidget *window)
{
  if ((!g_getenv ("SPOTIFY_SCROLL_STATS") && !g_getenv ("SPOTIFYGTK_LAYOUT_DIAGNOSTICS")) ||
      g_object_get_data (G_OBJECT (window), "frame-stats"))
    return;
  GdkFrameClock *clock = gtk_widget_get_frame_clock (window);
  if (!clock)
    return;
  FrameStats *stats = g_new0 (FrameStats, 1);
  stats->window = window;
  stats->trace_layout = g_getenv ("SPOTIFYGTK_LAYOUT_DIAGNOSTICS") != NULL;
  stats->clock = g_object_ref (clock);
  stats->started_us = g_get_monotonic_time ();
  stats->before_id = g_signal_connect (clock, "before-paint",
                                       G_CALLBACK (before_paint), stats);
  stats->after_id = g_signal_connect (clock, "after-paint",
                                      G_CALLBACK (after_paint), stats);
  g_object_set_data_full (G_OBJECT (window), "frame-stats", stats,
                          frame_stats_free);
}

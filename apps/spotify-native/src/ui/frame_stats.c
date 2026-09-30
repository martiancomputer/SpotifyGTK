#include "frame_stats.h"

typedef struct {
  GtkWidget *window; /* owned by the window's data slot */
  GdkFrameClock *clock;
  gulong before_id, after_id;
  gint64 started_us, paint_started_us, last_frame_us;
  gint64 total_paint_us, max_paint_us, max_gap_us;
  guint frames, late_frames;
  gint width, height;
} FrameStats;

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
    g_message ("frame-profile: renderer=%s size=%dx%d frames=%u"
               " paint-avg=%.2fms paint-max=%.2fms gap-max=%.2fms"
               " late=%u refresh=%.2fms",
               renderer ? G_OBJECT_TYPE_NAME (renderer) : "unavailable",
               width, height, stats->frames,
               stats->total_paint_us / (1000.0 * stats->frames),
               stats->max_paint_us / 1000.0, stats->max_gap_us / 1000.0,
               stats->late_frames, refresh / 1000.0);
    stats->started_us = now;
    stats->frames = stats->late_frames = 0;
    stats->total_paint_us = stats->max_paint_us = stats->max_gap_us = 0;
  }
}

void
spotifygtk_frame_stats_attach (GtkWidget *window)
{
  if (!g_getenv ("SPOTIFY_SCROLL_STATS") ||
      g_object_get_data (G_OBJECT (window), "frame-stats"))
    return;
  GdkFrameClock *clock = gtk_widget_get_frame_clock (window);
  if (!clock)
    return;
  FrameStats *stats = g_new0 (FrameStats, 1);
  stats->window = window;
  stats->clock = g_object_ref (clock);
  stats->started_us = g_get_monotonic_time ();
  stats->before_id = g_signal_connect (clock, "before-paint",
                                       G_CALLBACK (before_paint), stats);
  stats->after_id = g_signal_connect (clock, "after-paint",
                                      G_CALLBACK (after_paint), stats);
  g_object_set_data_full (G_OBJECT (window), "frame-stats", stats,
                          frame_stats_free);
}

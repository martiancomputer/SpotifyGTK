#pragma once
#include <gtk/gtk.h>

/* Passive frame measurements, enabled by SPOTIFY_SCROLL_STATS. */
void spotifygtk_frame_stats_attach (GtkWidget *window);

typedef enum {
  SPOTIFYGTK_FRAME_WORK_BIND,
  SPOTIFYGTK_FRAME_WORK_HEADER_BIND,
  SPOTIFYGTK_FRAME_WORK_GLASS,
  SPOTIFYGTK_FRAME_WORK_COUNT
} SpotifyGtkFrameWork;

/* Zero when diagnostics are off; no clock reads or retained per-row data. */
gint64 spotifygtk_frame_stats_work_begin (void);
void spotifygtk_frame_stats_work_end (GtkWidget *widget,
                                      SpotifyGtkFrameWork work, gint64 started);

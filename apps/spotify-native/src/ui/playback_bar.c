/*
 * playback_bar.c — Bottom playback bar implementation.
 */

#include "playback_bar.h"

struct _SpotifyGtkPlaybackBar {
  GtkBox parent_instance;

  /* Left: Album art + track info */
  GtkImage *album_art;
  GtkLabel *track_label;

  /* Center: Controls */
  GtkButton *prev_btn;
  GtkButton *play_btn;
  GtkButton *next_btn;

  /* Progress */
  GtkScale *progress_scale;

  /* State */
  gboolean is_playing;
  gint64 position_ms;
  gint64 duration_ms;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkPlaybackBar, spotifygtk_playback_bar, GTK_TYPE_BOX)

enum {
  PLAY_CLICKED,
  PAUSE_CLICKED,
  NEXT_CLICKED,
  PREV_CLICKED,
  SEEK,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
on_play_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  if (self->is_playing)
    g_signal_emit (self, signals[PAUSE_CLICKED], 0);
  else
    g_signal_emit (self, signals[PLAY_CLICKED], 0);
  (void) button;
}

static void
on_seek_changed (GtkRange *range, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  gdouble value = gtk_range_get_value (range);
  if (self->duration_ms > 0) {
    gint64 new_pos = (gint64) (value * (gdouble) self->duration_ms);
    g_signal_emit (self, signals[SEEK], 0, new_pos);
  }
  (void) range;
}

static void
spotifygtk_playback_bar_dispose (GObject *object)
{
  G_OBJECT_CLASS (spotifygtk_playback_bar_parent_class)->dispose (object);
}

static void
spotifygtk_playback_bar_class_init (SpotifyGtkPlaybackBarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_playback_bar_dispose;

  signals[PLAY_CLICKED] = g_signal_new ("play-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[PAUSE_CLICKED] = g_signal_new ("pause-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[NEXT_CLICKED] = g_signal_new ("next-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[PREV_CLICKED] = g_signal_new ("prev-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SEEK] = g_signal_new ("seek",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT64);
}

static void
spotifygtk_playback_bar_init (SpotifyGtkPlaybackBar *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "playback-bar");

  gtk_widget_set_margin_start (GTK_WIDGET (self), 16);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 16);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 12);

  /* === Left: Album art + track info === */
  self->album_art = GTK_IMAGE (gtk_image_new_from_icon_name ("audio-x-generic-symbolic"));
  gtk_image_set_pixel_size (self->album_art, 48);
  gtk_widget_add_css_class (GTK_WIDGET (self->album_art), "card");
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->album_art));

  self->track_label = GTK_LABEL (gtk_label_new ("Track Title"));
  gtk_widget_add_css_class (GTK_WIDGET (self->track_label), "normal-text");
  gtk_label_set_xalign (self->track_label, 0.0);
  gtk_widget_set_size_request (GTK_WIDGET (self->track_label), 120, -1);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->track_label));

  /* Spacer */
  GtkWidget *spacer1 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer1, TRUE);
  gtk_box_append (GTK_BOX (self), spacer1);

  /* === Center: Controls === */
  GtkWidget *center = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (center, GTK_ALIGN_CENTER);

  self->prev_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("media-skip-backward-symbolic"));
  gtk_widget_add_css_class (GTK_WIDGET (self->prev_btn), "flat");
  g_signal_connect_swapped (self->prev_btn, "clicked",
                            G_CALLBACK (g_signal_emit_by_name), self);
  gtk_box_append (GTK_BOX (center), GTK_WIDGET (self->prev_btn));

  self->play_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("media-playback-start-symbolic"));
  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "circular");
  g_signal_connect (self->play_btn, "clicked", G_CALLBACK (on_play_clicked), self);
  gtk_box_append (GTK_BOX (center), GTK_WIDGET (self->play_btn));

  self->next_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("media-skip-forward-symbolic"));
  gtk_widget_add_css_class (GTK_WIDGET (self->next_btn), "flat");
  g_signal_connect_swapped (self->next_btn, "clicked",
                            G_CALLBACK (g_signal_emit_by_name), self);
  gtk_box_append (GTK_BOX (center), GTK_WIDGET (self->next_btn));

  gtk_box_append (GTK_BOX (self), center);

  /* Spacer */
  GtkWidget *spacer2 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer2, TRUE);
  gtk_box_append (GTK_BOX (self), spacer2);

  /* === Right: Progress bar === */
  self->progress_scale = GTK_SCALE (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.001));
  gtk_scale_set_draw_value (self->progress_scale, FALSE);
  gtk_widget_set_size_request (GTK_WIDGET (self->progress_scale), 400, -1);
  g_signal_connect (self->progress_scale, "value-changed", G_CALLBACK (on_seek_changed), self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->progress_scale));
}

SpotifyGtkPlaybackBar *
spotifygtk_playback_bar_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_PLAYBACK_BAR, NULL);
}

void
spotifygtk_playback_bar_set_track (SpotifyGtkPlaybackBar *self,
                                   const gchar *track_name,
                                   const gchar *artist)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));
  gtk_label_set_text (self->track_label, track_name ? track_name : "Not Playing");
  (void) artist;
}

void
spotifygtk_playback_bar_set_playing (SpotifyGtkPlaybackBar *self, gboolean is_playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));
  self->is_playing = is_playing;
  const gchar *icon = is_playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic";
  gtk_button_set_icon_name (self->play_btn, icon);
}

void
spotifygtk_playback_bar_set_progress (SpotifyGtkPlaybackBar *self,
                                      gint64 position_ms,
                                      gint64 duration_ms)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));
  self->position_ms = position_ms;
  self->duration_ms = duration_ms;

  if (duration_ms > 0) {
    gdouble fraction = (gdouble) position_ms / (gdouble) duration_ms;
    gtk_range_set_value (GTK_RANGE (self->progress_scale), CLAMP (fraction, 0.0, 1.0));
  }
}

void
spotifygtk_playback_bar_set_queue_count (SpotifyGtkPlaybackBar *self, gint count)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));
  (void) count;
}

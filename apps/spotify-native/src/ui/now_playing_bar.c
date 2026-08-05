/*
 * now_playing_bar.c — Persistent playback bar implementation.
 */

#include "now_playing_bar.h"

struct _SpotifyGtkNowPlayingBar {
  GtkWidget parent_instance;

  GtkBox *root_box;

  /* Left: Album art + track info */
  GtkImage *album_art;
  GtkLabel *track_label;
  GtkLabel *artist_label;
  GtkWidget *like_button;

  /* Center: Controls + progress */
  GtkButton *shuffle_btn;
  GtkButton *prev_btn;
  GtkButton *play_pause_btn;
  GtkButton *next_btn;
  GtkButton *repeat_btn;
  GtkScale *progress_scale;
  GtkLabel *position_label;
  GtkLabel *duration_label;

  /* Right: Volume */
  GtkScale *volume_scale;
  GtkButton *queue_btn;

  /* State */
  gboolean is_playing;
  gint64 position_ms;
  gint64 duration_ms;
  gint volume;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkNowPlayingBar, spotifygtk_now_playing_bar, GTK_TYPE_WIDGET)

enum {
  PLAY_CLICKED,
  PAUSE_CLICKED,
  NEXT_CLICKED,
  PREVIOUS_CLICKED,
  SEEK,
  VOLUME_CHANGED,
  SHUFFLE_CLICKED,
  REPEAT_CLICKED,
  QUEUE_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static gchar *
format_time (gint64 ms)
{
  gint total_secs = (gint) (ms / 1000);
  gint mins = total_secs / 60;
  gint secs = total_secs % 60;
  return g_strdup_printf ("%d:%02d", mins, secs);
}

static void
update_play_button_icon (SpotifyGtkNowPlayingBar *self)
{
  const gchar *icon_name = self->is_playing ?
    "media-playback-pause-symbolic" : "media-playback-start-symbolic";
  gtk_button_set_icon_name (self->play_pause_btn, icon_name);
}

static void
on_play_pause_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkNowPlayingBar *self = SPOTIFYGTK_NOW_PLAYING_BAR (user_data);
  if (self->is_playing)
    g_signal_emit (self, signals[PAUSE_CLICKED], 0);
  else
    g_signal_emit (self, signals[PLAY_CLICKED], 0);
}

static void
on_seek_changed (GtkRange *range, gpointer user_data)
{
  SpotifyGtkNowPlayingBar *self = SPOTIFYGTK_NOW_PLAYING_BAR (user_data);
  gdouble value = gtk_range_get_value (range);
  if (self->duration_ms > 0) {
    gint64 new_pos = (gint64) (value * (gdouble) self->duration_ms);
    g_signal_emit (self, signals[SEEK], 0, new_pos);
  }
  (void) range;
}

static void
on_volume_changed (GtkRange *range, gpointer user_data)
{
  SpotifyGtkNowPlayingBar *self = SPOTIFYGTK_NOW_PLAYING_BAR (user_data);
  self->volume = (gint) gtk_range_get_value (range);
  g_signal_emit (self, signals[VOLUME_CHANGED], 0, self->volume);
}

static GtkWidget *
create_icon_button (const gchar *icon_name, const gchar *tooltip)
{
  GtkWidget *btn = gtk_button_new_from_icon_name (icon_name);
  gtk_widget_set_tooltip_text (btn, tooltip);
  gtk_widget_add_css_class (btn, "flat");
  return btn;
}

static void
spotifygtk_now_playing_bar_dispose (GObject *object)
{
  SpotifyGtkNowPlayingBar *self = SPOTIFYGTK_NOW_PLAYING_BAR (object);
  gtk_widget_unparent (GTK_WIDGET (self->root_box));
  G_OBJECT_CLASS (spotifygtk_now_playing_bar_parent_class)->dispose (object);
}

static void
spotifygtk_now_playing_bar_class_init (SpotifyGtkNowPlayingBarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_now_playing_bar_dispose;

  signals[PLAY_CLICKED] = g_signal_new ("play-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[PAUSE_CLICKED] = g_signal_new ("pause-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[NEXT_CLICKED] = g_signal_new ("next-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[PREVIOUS_CLICKED] = g_signal_new ("previous-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SEEK] = g_signal_new ("seek",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT64);

  signals[VOLUME_CHANGED] = g_signal_new ("volume-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT);

  signals[SHUFFLE_CLICKED] = g_signal_new ("shuffle-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[REPEAT_CLICKED] = g_signal_new ("repeat-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[QUEUE_CLICKED] = g_signal_new ("queue-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void
spotifygtk_now_playing_bar_init (SpotifyGtkNowPlayingBar *self)
{
  self->root_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_add_css_class (GTK_WIDGET (self->root_box), "now-playing-bar");
  gtk_widget_add_css_class (GTK_WIDGET (self->root_box), "toolbar");

  gtk_widget_set_margin_start (GTK_WIDGET (self->root_box), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self->root_box), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (self->root_box), 8);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->root_box), 8);

  /* === LEFT: Track info === */
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_size_request (left, 240, -1);

  self->album_art = GTK_IMAGE (gtk_image_new_from_icon_name ("audio-x-generic-symbolic"));
  gtk_image_set_pixel_size (self->album_art, 56);
  gtk_widget_add_css_class (GTK_WIDGET (self->album_art), "card");
  gtk_box_append (GTK_BOX (left), GTK_WIDGET (self->album_art));

  GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  self->track_label = GTK_LABEL (gtk_label_new ("Not Playing"));
  self->artist_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (self->track_label, 0.0);
  gtk_label_set_xalign (self->artist_label, 0.0);
  gtk_label_set_ellipsize (self->track_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize (self->artist_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (self->track_label), "heading");
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "caption");
  gtk_box_append (GTK_BOX (info_box), GTK_WIDGET (self->track_label));
  gtk_box_append (GTK_BOX (info_box), GTK_WIDGET (self->artist_label));

  self->like_button = create_icon_button ("spotifygtk-heart-outline-symbolic", "Add to Liked Songs");
  gtk_box_append (GTK_BOX (info_box), self->like_button);

  gtk_box_append (GTK_BOX (left), info_box);
  gtk_box_append (GTK_BOX (self->root_box), left);

  /* === CENTER: Controls + Progress === */
  GtkWidget *center = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand (center, TRUE);
  gtk_widget_set_halign (center, GTK_ALIGN_CENTER);

  /* Controls row */
  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (controls, GTK_ALIGN_CENTER);

  self->shuffle_btn = GTK_BUTTON (create_icon_button ("media-playlist-shuffle-symbolic", "Shuffle"));
  self->prev_btn = GTK_BUTTON (create_icon_button ("media-skip-backward-symbolic", "Previous"));
  self->play_pause_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("media-playback-start-symbolic"));
  self->next_btn = GTK_BUTTON (create_icon_button ("media-skip-forward-symbolic", "Next"));
  self->repeat_btn = GTK_BUTTON (create_icon_button ("media-playlist-repeat-symbolic", "Repeat"));

  gtk_widget_add_css_class (GTK_WIDGET (self->play_pause_btn), "circular");
  gtk_widget_add_css_class (GTK_WIDGET (self->play_pause_btn), "suggested-action");

  g_signal_connect (self->play_pause_btn, "clicked", G_CALLBACK (on_play_pause_clicked), self);
  g_signal_connect_swapped (self->next_btn, "clicked", G_CALLBACK (g_signal_emit_by_name), self);
  g_signal_connect_swapped (self->prev_btn, "clicked", G_CALLBACK (g_signal_emit_by_name), self);
  g_signal_connect_swapped (self->shuffle_btn, "clicked", G_CALLBACK (g_signal_emit_by_name), self);
  g_signal_connect_swapped (self->repeat_btn, "clicked", G_CALLBACK (g_signal_emit_by_name), self);

  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->shuffle_btn));
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->prev_btn));
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->play_pause_btn));
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->next_btn));
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->repeat_btn));

  /* Progress row */
  GtkWidget *progress_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->position_label = GTK_LABEL (gtk_label_new ("0:00"));
  self->progress_scale = GTK_SCALE (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.001));
  self->duration_label = GTK_LABEL (gtk_label_new ("0:00"));

  gtk_scale_set_draw_value (self->progress_scale, FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->progress_scale), TRUE);
  gtk_widget_set_size_request (GTK_WIDGET (self->progress_scale), 400, -1);

  gtk_widget_add_css_class (GTK_WIDGET (self->position_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->duration_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->position_label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->duration_label), "dim-label");

  g_signal_connect (self->progress_scale, "value-changed", G_CALLBACK (on_seek_changed), self);

  gtk_box_append (GTK_BOX (progress_row), GTK_WIDGET (self->position_label));
  gtk_box_append (GTK_BOX (progress_row), GTK_WIDGET (self->progress_scale));
  gtk_box_append (GTK_BOX (progress_row), GTK_WIDGET (self->duration_label));

  gtk_box_append (GTK_BOX (center), controls);
  gtk_box_append (GTK_BOX (center), progress_row);
  gtk_box_append (GTK_BOX (self->root_box), center);

  /* === RIGHT: Volume + Queue === */
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_size_request (right, 200, -1);

  GtkWidget *vol_icon = gtk_image_new_from_icon_name ("audio-volume-high-symbolic");
  self->volume_scale = GTK_SCALE (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0));
  gtk_scale_set_draw_value (self->volume_scale, FALSE);
  gtk_range_set_value (GTK_RANGE (self->volume_scale), 50.0);
  gtk_widget_set_hexpand (GTK_WIDGET (self->volume_scale), TRUE);

  g_signal_connect (self->volume_scale, "value-changed", G_CALLBACK (on_volume_changed), self);

  self->queue_btn = GTK_BUTTON (create_icon_button ("view-list-symbolic", "Queue"));
  g_signal_connect_swapped (self->queue_btn, "clicked", G_CALLBACK (g_signal_emit_by_name), self);

  gtk_box_append (GTK_BOX (right), vol_icon);
  gtk_box_append (GTK_BOX (right), GTK_WIDGET (self->volume_scale));
  gtk_box_append (GTK_BOX (right), GTK_WIDGET (self->queue_btn));
  gtk_box_append (GTK_BOX (self->root_box), right);

  gtk_widget_set_parent (GTK_WIDGET (self->root_box), GTK_WIDGET (self));

  self->volume = 50;
  self->is_playing = FALSE;
}

SpotifyGtkNowPlayingBar *
spotifygtk_now_playing_bar_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_NOW_PLAYING_BAR, NULL);
}

void
spotifygtk_now_playing_bar_set_track (SpotifyGtkNowPlayingBar *self,
                                      const gchar *track_name,
                                      const gchar *artist_name)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_BAR (self));
  gtk_label_set_text (self->track_label, track_name ? track_name : "Not Playing");
  gtk_label_set_text (self->artist_label, artist_name ? artist_name : "");
}

void
spotifygtk_now_playing_bar_set_album_art (SpotifyGtkNowPlayingBar *self,
                                          const gchar *image_path)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_BAR (self));
  if (image_path && *image_path) {
    /* TODO: Use image_cache when wired */
    gtk_image_set_from_icon_name (self->album_art, "audio-x-generic-symbolic");
  } else {
    gtk_image_set_from_icon_name (self->album_art, "audio-x-generic-symbolic");
  }
}

void
spotifygtk_now_playing_bar_set_playing (SpotifyGtkNowPlayingBar *self, gboolean is_playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_BAR (self));
  self->is_playing = is_playing;
  update_play_button_icon (self);
}

void
spotifygtk_now_playing_bar_set_progress (SpotifyGtkNowPlayingBar *self,
                                         gint64 position_ms,
                                         gint64 duration_ms)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_BAR (self));

  self->position_ms = position_ms;
  self->duration_ms = duration_ms;

  g_autofree gchar *pos_str = format_time (position_ms);
  g_autofree gchar *dur_str = format_time (duration_ms);

  gtk_label_set_text (self->position_label, pos_str);
  gtk_label_set_text (self->duration_label, dur_str);

  if (duration_ms > 0) {
    gdouble fraction = (gdouble) position_ms / (gdouble) duration_ms;
    gtk_range_set_value (GTK_RANGE (self->progress_scale), CLAMP (fraction, 0.0, 1.0));
  }
}

void
spotifygtk_now_playing_bar_set_volume (SpotifyGtkNowPlayingBar *self, gint volume_percent)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_BAR (self));
  self->volume = CLAMP (volume_percent, 0, 100);
  gtk_range_set_value (GTK_RANGE (self->volume_scale), (gdouble) self->volume);
}

gint
spotifygtk_now_playing_bar_get_volume (SpotifyGtkNowPlayingBar *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_BAR (self), 50);
  return self->volume;
}

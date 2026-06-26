#include "config.h"
#include "now_playing.h"

struct _SpotifyNowPlayingBar {
  GtkBox     parent_instance;

  GtkImage  *album_art;
  GtkLabel  *track_label;
  GtkLabel  *artist_label;
  GtkButton *prev_btn;
  GtkButton *play_btn;
  GtkButton *next_btn;
  GtkScale  *progress;
  GtkLabel  *time_label;
  GtkScale  *volume;
};

G_DEFINE_FINAL_TYPE (SpotifyNowPlayingBar, spotifygtk_now_playing_bar, GTK_TYPE_BOX)

static gchar *
ms_to_time_str (gint64 ms)
{
  gint64 secs = ms / 1000;
  gint64 mins = secs / 60;
  return g_strdup_printf ("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, mins, secs % 60);
}

static void
spotifygtk_now_playing_bar_constructed (GObject *object)
{
  SpotifyNowPlayingBar *self = SPOTIFYGTK_NOW_PLAYING_BAR (object);
  G_OBJECT_CLASS (spotifygtk_now_playing_bar_parent_class)->constructed (object);

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_start  (GTK_WIDGET (self), 16);
  gtk_widget_set_margin_end    (GTK_WIDGET (self), 16);
  gtk_widget_set_margin_top    (GTK_WIDGET (self), 8);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 8);
  gtk_box_set_spacing          (GTK_BOX (self), 16);
  gtk_widget_add_css_class     (GTK_WIDGET (self), "now-playing-bar");

  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_size_request (left, 280, -1);

  self->album_art = GTK_IMAGE (gtk_image_new_from_icon_name ("audio-x-generic-symbolic"));
  gtk_image_set_pixel_size (self->album_art, 48);

  GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  self->track_label  = GTK_LABEL (gtk_label_new ("Not Playing"));
  self->artist_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_ellipsize (self->track_label,  PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize (self->artist_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (self->track_label),  "title-4");
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "dim-label");

  gtk_box_append (GTK_BOX (info_box), GTK_WIDGET (self->track_label));
  gtk_box_append (GTK_BOX (info_box), GTK_WIDGET (self->artist_label));
  gtk_box_append (GTK_BOX (left), GTK_WIDGET (self->album_art));
  gtk_box_append (GTK_BOX (left), info_box);

  GtkWidget *center = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand (center, TRUE);
  gtk_widget_set_halign  (center, GTK_ALIGN_CENTER);

  GtkWidget *ctrl_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (ctrl_row, GTK_ALIGN_CENTER);

  self->prev_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("media-skip-backward-symbolic"));
  self->play_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("media-playback-start-symbolic"));
  self->next_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("media-skip-forward-symbolic"));

  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "circular");
  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "suggested-action");

  gtk_box_append (GTK_BOX (ctrl_row), GTK_WIDGET (self->prev_btn));
  gtk_box_append (GTK_BOX (ctrl_row), GTK_WIDGET (self->play_btn));
  gtk_box_append (GTK_BOX (ctrl_row), GTK_WIDGET (self->next_btn));

  GtkWidget *prog_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->time_label = GTK_LABEL (gtk_label_new ("0:00 / 0:00"));
  gtk_widget_add_css_class (GTK_WIDGET (self->time_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->time_label), "dim-label");

  self->progress = GTK_SCALE (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.001));
  gtk_scale_set_draw_value   (self->progress, FALSE);
  gtk_widget_set_hexpand     (GTK_WIDGET (self->progress), TRUE);
  gtk_widget_set_size_request (GTK_WIDGET (self->progress), 400, -1);

  gtk_box_append (GTK_BOX (prog_row), GTK_WIDGET (self->progress));
  gtk_box_append (GTK_BOX (prog_row), GTK_WIDGET (self->time_label));

  gtk_box_append (GTK_BOX (center), ctrl_row);
  gtk_box_append (GTK_BOX (center), prog_row);

  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_size_request (right, 200, -1);
  gtk_widget_set_halign  (right, GTK_ALIGN_END);

  GtkWidget *vol_icon = gtk_image_new_from_icon_name ("audio-volume-medium-symbolic");
  self->volume = GTK_SCALE (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1));
  gtk_scale_set_draw_value (self->volume, FALSE);
  gtk_range_set_value      (GTK_RANGE (self->volume), 50);
  gtk_widget_set_hexpand   (GTK_WIDGET (self->volume), TRUE);

  gtk_box_append (GTK_BOX (right), vol_icon);
  gtk_box_append (GTK_BOX (right), GTK_WIDGET (self->volume));

  gtk_box_append (GTK_BOX (self), left);
  gtk_box_append (GTK_BOX (self), center);
  gtk_box_append (GTK_BOX (self), right);
}

void
spotifygtk_now_playing_bar_update (SpotifyNowPlayingBar *self, const gchar *track, const gchar *artist,
                                   gboolean is_playing, gint64 position_ms, gint64 duration_ms)
{
  gtk_label_set_text (self->track_label,  track  ? track  : "");
  gtk_label_set_text (self->artist_label, artist ? artist : "");

  gtk_button_set_icon_name (self->play_btn,
    is_playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");

  if (duration_ms > 0)
    gtk_range_set_value (GTK_RANGE (self->progress), (gdouble) position_ms / (gdouble) duration_ms);

  g_autofree gchar *pos_str = ms_to_time_str (position_ms);
  g_autofree gchar *dur_str = ms_to_time_str (duration_ms);
  g_autofree gchar *time    = g_strdup_printf ("%s / %s", pos_str, dur_str);
  gtk_label_set_text (self->time_label, time);
}

static void spotifygtk_now_playing_bar_class_init (SpotifyNowPlayingBarClass *klass)
{
  G_OBJECT_CLASS (klass)->constructed = spotifygtk_now_playing_bar_constructed;
}

static void spotifygtk_now_playing_bar_init (SpotifyNowPlayingBar *self) { (void) self; }

SpotifyNowPlayingBar *
spotifygtk_now_playing_bar_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_NOW_PLAYING_BAR, NULL);
}

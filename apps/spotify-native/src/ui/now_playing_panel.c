/*
 * now_playing_panel.c — Right-side Now Playing panel implementation.
 */

#include "now_playing_panel.h"

struct _SpotifyGtkNowPlayingPanel {
  GtkBox parent_instance;

  GtkImage *album_art;
  GtkLabel *track_label;
  GtkLabel *artist_label;
  GtkListBox *queue_list;
  GtkWidget  *art_section;   /* artwork + track info; what collapses */
  GtkButton  *collapse_btn;
  gboolean    collapsed;

  gboolean is_playing;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkNowPlayingPanel, spotifygtk_now_playing_panel, GTK_TYPE_BOX)

static void
spotifygtk_now_playing_panel_dispose (GObject *object)
{
  G_OBJECT_CLASS (spotifygtk_now_playing_panel_parent_class)->dispose (object);
}

static void
spotifygtk_now_playing_panel_class_init (SpotifyGtkNowPlayingPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_now_playing_panel_dispose;
}

/* Collapsing hides the artwork and track info, leaving the header and the
 * queue. That is what the arrow in the mockup implies and what makes the
 * panel useful on a short window. */
static void
on_collapse_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkNowPlayingPanel *self = user_data;

  self->collapsed = !self->collapsed;
  gtk_widget_set_visible (self->art_section, !self->collapsed);
  gtk_button_set_label (self->collapse_btn,
                        self->collapsed ? "Expand ▸" : "Collapse ◂");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->collapse_btn),
                               self->collapsed ? "Show the artwork" : "Hide the artwork");
  (void) button;
}

static void
spotifygtk_now_playing_panel_init (SpotifyGtkNowPlayingPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "now-playing-panel");
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);

  /* Header: "Now Playing" + Collapse */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_start (header, 16);
  gtk_widget_set_margin_end (header, 16);
  gtk_widget_set_margin_top (header, 16);
  gtk_widget_set_margin_bottom (header, 8);

  GtkWidget *title = gtk_label_new ("Now Playing");
  gtk_widget_add_css_class (title, "normal-text");
  gtk_widget_set_hexpand (title, TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (header), title);

  /* This was a GtkLabel, which is why clicking it did nothing. It has to be
   * an actual button to be activatable at all. */
  self->collapse_btn = GTK_BUTTON (gtk_button_new_with_label ("Collapse ◂"));
  gtk_widget_add_css_class (GTK_WIDGET (self->collapse_btn), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (self->collapse_btn), "dim-text");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->collapse_btn), "Hide the artwork");
  g_signal_connect (self->collapse_btn, "clicked", G_CALLBACK (on_collapse_clicked), self);
  gtk_box_append (GTK_BOX (header), GTK_WIDGET (self->collapse_btn));

  gtk_box_append (GTK_BOX (self), header);

  /* Artwork and track info live in one box so the collapse button has a
   * single thing to hide. */
  self->art_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  self->album_art = GTK_IMAGE (gtk_image_new_from_icon_name ("audio-x-generic-symbolic"));
  gtk_image_set_pixel_size (self->album_art, 96);
  gtk_widget_set_size_request (GTK_WIDGET (self->album_art), 220, 220);
  gtk_widget_add_css_class (GTK_WIDGET (self->album_art), "art-large");
  gtk_widget_set_halign (GTK_WIDGET (self->album_art), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (GTK_WIDGET (self->album_art), 24);
  gtk_widget_set_margin_end (GTK_WIDGET (self->album_art), 24);
  gtk_widget_set_margin_top (GTK_WIDGET (self->album_art), 8);
  gtk_box_append (GTK_BOX (self->art_section), GTK_WIDGET (self->album_art));

  /* Track info */
  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start (info, 24);
  gtk_widget_set_margin_end (info, 24);
  gtk_widget_set_margin_top (info, 16);

  self->track_label = GTK_LABEL (gtk_label_new ("Track Title"));
  gtk_widget_add_css_class (GTK_WIDGET (self->track_label), "normal-text");
  gtk_label_set_xalign (self->track_label, 0.0);
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->track_label));

  self->artist_label = GTK_LABEL (gtk_label_new ("Artist • Album"));
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "dim-text");
  gtk_label_set_xalign (self->artist_label, 0.0);
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->artist_label));

  gtk_box_append (GTK_BOX (self->art_section), info);
  gtk_box_append (GTK_BOX (self), self->art_section);

  /* Queue list */
  self->queue_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_set_margin_top (GTK_WIDGET (self->queue_list), 16);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->queue_list));
}

SpotifyGtkNowPlayingPanel *
spotifygtk_now_playing_panel_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_NOW_PLAYING_PANEL, NULL);
}

void
spotifygtk_now_playing_panel_set_track (SpotifyGtkNowPlayingPanel *self,
                                        const gchar *track_name,
                                        const gchar *artist,
                                        const gchar *album)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));

  gtk_label_set_text (self->track_label, track_name ? track_name : "");

  g_autofree gchar *subtitle = g_strdup_printf ("%s • %s",
                                                artist ? artist : "",
                                                album ? album : "");
  gtk_label_set_text (self->artist_label, subtitle);
}

void
spotifygtk_now_playing_panel_set_album_art (SpotifyGtkNowPlayingPanel *self,
                                            const gchar *image_path)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));
  if (image_path && *image_path) {
    /* TODO: Load actual image */
  }
  (void) image_path;
}

void
spotifygtk_now_playing_panel_set_playing (SpotifyGtkNowPlayingPanel *self, gboolean is_playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));
  self->is_playing = is_playing;
}

void
spotifygtk_now_playing_panel_set_progress (SpotifyGtkNowPlayingPanel *self,
                                           gint64 position_ms,
                                           gint64 duration_ms)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));
  /* Progress is shown in playback bar, not here in mockup */
  (void) self; (void) position_ms; (void) duration_ms;
}

void
spotifygtk_now_playing_panel_set_queue (SpotifyGtkNowPlayingPanel *self, JsonArray *tracks)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));

  /* Clear existing */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->queue_list))))
    gtk_list_box_remove (self->queue_list, child);

  if (!tracks) return;

  for (guint i = 0; i < json_array_get_length (tracks); i++) {
    JsonObject *track = json_array_get_object_element (tracks, i);
    const gchar *name = json_object_get_string_member_with_default (track, "name", "");

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_widget_add_css_class (row, "list-row");

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_widget_set_margin_top (box, 12);
    gtk_widget_set_margin_bottom (box, 12);

    GtkWidget *label = gtk_label_new (name);
    gtk_widget_add_css_class (label, "normal-text");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_append (GTK_BOX (box), label);

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_list_box_append (self->queue_list, row);
  }
}

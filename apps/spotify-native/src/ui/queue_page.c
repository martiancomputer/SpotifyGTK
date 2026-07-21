/*
 * queue_page.c — Playback queue view implementation.
 */

#include "queue_page.h"

struct _SpotifyGtkQueuePage {
  AdwBin parent_instance;

  GtkBox *root_box;
  GtkListBox *now_playing_box;
  GtkListBox *queue_list;
  GtkButton *clear_btn;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkQueuePage, spotifygtk_queue_page, ADW_TYPE_BIN)

enum {
  PLAY_TRACK,
  REMOVE_TRACK,
  CLEAR_QUEUE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
spotifygtk_queue_page_class_init (SpotifyGtkQueuePageClass *klass)
{
  signals[PLAY_TRACK] = g_signal_new ("play-track",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT);

  signals[REMOVE_TRACK] = g_signal_new ("remove-track",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT);

  signals[CLEAR_QUEUE] = g_signal_new ("clear-queue",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void
spotifygtk_queue_page_init (SpotifyGtkQueuePage *self)
{
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  self->root_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 24));
  gtk_widget_set_margin_start (GTK_WIDGET (self->root_box), 24);
  gtk_widget_set_margin_end (GTK_WIDGET (self->root_box), 24);
  gtk_widget_set_margin_top (GTK_WIDGET (self->root_box), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->root_box), 24);

  /* Title */
  GtkWidget *title = gtk_label_new ("Queue");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_widget_add_css_class (title, "title-1");
  gtk_box_append (GTK_BOX (self->root_box), title);

  /* Now Playing section */
  GtkWidget *now_playing_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *np_title = gtk_label_new ("Now Playing");
  gtk_label_set_xalign (GTK_LABEL (np_title), 0.0);
  gtk_widget_add_css_class (np_title, "title-3");
  gtk_box_append (GTK_BOX (now_playing_section), np_title);

  self->now_playing_box = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->now_playing_box), "boxed-list");
  gtk_box_append (GTK_BOX (now_playing_section), GTK_WIDGET (self->now_playing_box));
  gtk_box_append (GTK_BOX (self->root_box), now_playing_section);

  /* Queue section */
  GtkWidget *queue_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *queue_header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand (queue_header, TRUE);

  GtkWidget *queue_title = gtk_label_new ("Next Up");
  gtk_label_set_xalign (GTK_LABEL (queue_title), 0.0);
  gtk_widget_add_css_class (queue_title, "title-3");
  gtk_widget_set_hexpand (queue_title, TRUE);
  gtk_box_append (GTK_BOX (queue_header), queue_title);

  self->clear_btn = GTK_BUTTON (gtk_button_new_with_label ("Clear"));
  gtk_widget_add_css_class (GTK_WIDGET (self->clear_btn), "destructive-action");
  gtk_box_append (GTK_BOX (queue_header), GTK_WIDGET (self->clear_btn));
  gtk_box_append (GTK_BOX (queue_section), queue_header);

  self->queue_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->queue_list), "boxed-list");
  gtk_box_append (GTK_BOX (queue_section), GTK_WIDGET (self->queue_list));
  gtk_box_append (GTK_BOX (self->root_box), queue_section);

  /* Placeholder */
  GtkWidget *placeholder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign (placeholder, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (placeholder, GTK_ALIGN_CENTER);
  GtkWidget *ph_title = gtk_label_new ("No tracks in queue");
  gtk_widget_add_css_class (ph_title, "title-2");
  GtkWidget *ph_desc = gtk_label_new ("Add tracks to the queue by right-clicking or using the context menu");
  gtk_widget_add_css_class (ph_desc, "dim-label");
  gtk_box_append (GTK_BOX (placeholder), ph_title);
  gtk_box_append (GTK_BOX (placeholder), ph_desc);
  gtk_box_append (GTK_BOX (self->root_box), placeholder);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (self->root_box));
  adw_bin_set_child (ADW_BIN (self), scroll);
}

SpotifyGtkQueuePage *
spotifygtk_queue_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_QUEUE_PAGE, NULL);
}

void
spotifygtk_queue_page_set_now_playing (SpotifyGtkQueuePage *self, JsonObject *track)
{
  g_return_if_fail (SPOTIFYGTK_IS_QUEUE_PAGE (self));

  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->now_playing_box));
  if (child) gtk_list_box_remove (self->now_playing_box, child);

  if (!track) return;

  const gchar *name = json_object_get_string_member_with_default (track, "name", "Unknown");
  g_autofree gchar *artist = NULL;
  JsonArray *artists = json_object_has_member (track, "artists") ?
    json_object_get_array_member (track, "artists") : NULL;
  if (artists && json_array_get_length (artists) > 0) {
    JsonObject *first = json_array_get_object_element (artists, 0);
    artist = g_strdup (json_object_get_string_member_with_default (first, "name", ""));
  }

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);

  GtkWidget *playing_icon = gtk_image_new_from_icon_name ("media-playback-start-symbolic");
  gtk_widget_add_css_class (playing_icon, "accent");
  gtk_box_append (GTK_BOX (box), playing_icon);

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *title = gtk_label_new (name);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_widget_add_css_class (title, "heading");
  gtk_box_append (GTK_BOX (info), title);

  if (artist) {
    GtkWidget *artist_label = gtk_label_new (artist);
    gtk_label_set_xalign (GTK_LABEL (artist_label), 0.0);
    gtk_widget_add_css_class (artist_label, "dim-label");
    gtk_box_append (GTK_BOX (info), artist_label);
  }
  gtk_box_append (GTK_BOX (box), info);

  gtk_list_box_append (self->now_playing_box, box);
}

void
spotifygtk_queue_page_set_queue (SpotifyGtkQueuePage *self, JsonArray *tracks)
{
  g_return_if_fail (SPOTIFYGTK_IS_QUEUE_PAGE (self));

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->queue_list))))
    gtk_list_box_remove (self->queue_list, child);

  if (!tracks) return;

  for (guint i = 0; i < json_array_get_length (tracks); i++) {
    JsonObject *track = json_array_get_object_element (tracks, i);
    const gchar *name = json_object_get_string_member_with_default (track, "name", "Unknown");

    g_autofree gchar *artist = NULL;
    JsonArray *artists = json_object_has_member (track, "artists") ?
      json_object_get_array_member (track, "artists") : NULL;
    if (artists && json_array_get_length (artists) > 0) {
      JsonObject *first = json_array_get_object_element (artists, 0);
      artist = g_strdup (json_object_get_string_member_with_default (first, "name", ""));
    }

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_widget_set_margin_top (box, 8);
    gtk_widget_set_margin_bottom (box, 8);

    gchar *num_text = g_strdup_printf ("%u", i + 1);
    GtkWidget *num = gtk_label_new (num_text);
    g_free (num_text);
    gtk_widget_add_css_class (num, "dim-label");
    gtk_widget_set_size_request (num, 24, -1);
    gtk_box_append (GTK_BOX (box), num);

    GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title = gtk_label_new (name);
    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_box_append (GTK_BOX (info), title);

    if (artist) {
      GtkWidget *artist_label = gtk_label_new (artist);
      gtk_label_set_xalign (GTK_LABEL (artist_label), 0.0);
      gtk_widget_add_css_class (artist_label, "dim-label");
      gtk_widget_add_css_class (artist_label, "caption");
      gtk_box_append (GTK_BOX (info), artist_label);
    }
    gtk_box_append (GTK_BOX (box), info);

    gtk_list_box_append (self->queue_list, box);
  }
}

void
spotifygtk_queue_page_clear (SpotifyGtkQueuePage *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_QUEUE_PAGE (self));

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->queue_list))))
    gtk_list_box_remove (self->queue_list, child);
}

/*
 * artist_page.c — Artist detail view implementation.
 */

#include "artist_page.h"

struct _SpotifyGtkArtistPage {
  AdwBin parent_instance;

  GtkBox *root_box;
  GtkImage *artist_image;
  GtkLabel *name_label;
  GtkButton *follow_btn;
  GtkListBox *top_tracks_list;
  GtkListBox *albums_list;
  GtkListBox *related_list;

  gchar *artist_id;
  gboolean is_following;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkArtistPage, spotifygtk_artist_page, ADW_TYPE_BIN)

enum {
  PLAY_TRACK,
  PLAY_ARTIST_TOP,
  TOGGLE_FOLLOW,
  ALBUM_ACTIVATED,
  RELATED_ARTIST_ACTIVATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static gchar *
duration_str (gint ms)
{
  gint total_secs = ms / 1000;
  return g_strdup_printf ("%d:%02d", total_secs / 60, total_secs % 60);
}

static void
on_track_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  const gchar *uri = g_object_get_data (G_OBJECT (row), "track-uri");
  if (uri)
    g_signal_emit (self, signals[PLAY_TRACK], 0, uri);
  (void) box;
}

static void
on_album_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  const gchar *id = g_object_get_data (G_OBJECT (row), "album-id");
  if (id)
    g_signal_emit (self, signals[ALBUM_ACTIVATED], 0, id);
  (void) box;
}

static void
on_related_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  const gchar *id = g_object_get_data (G_OBJECT (row), "artist-id");
  if (id)
    g_signal_emit (self, signals[RELATED_ARTIST_ACTIVATED], 0, id);
  (void) box;
}

static void
on_follow_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  self->is_following = !self->is_following;
  gtk_button_set_label (self->follow_btn,
                        self->is_following ? "Following" : "Follow");
  if (self->artist_id)
    g_signal_emit (self, signals[TOGGLE_FOLLOW], 0, self->artist_id, self->is_following);
  (void) button;
}

static void
spotifygtk_artist_page_dispose (GObject *object)
{
  SpotifyGtkArtistPage *self = SPOTIFYGTK_ARTIST_PAGE (object);
  g_clear_pointer (&self->artist_id, g_free);
  G_OBJECT_CLASS (spotifygtk_artist_page_parent_class)->dispose (object);
}

static void
spotifygtk_artist_page_class_init (SpotifyGtkArtistPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_artist_page_dispose;

  signals[PLAY_TRACK] = g_signal_new ("play-track",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[PLAY_ARTIST_TOP] = g_signal_new ("play-artist-top",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[TOGGLE_FOLLOW] = g_signal_new ("toggle-follow",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  signals[ALBUM_ACTIVATED] = g_signal_new ("album-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[RELATED_ARTIST_ACTIVATED] = g_signal_new ("related-artist-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clear_list (GtkListBox *list)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (list))))
    gtk_list_box_remove (list, child);
}

static void
spotifygtk_artist_page_init (SpotifyGtkArtistPage *self)
{
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  self->root_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 24));
  gtk_widget_set_margin_start (GTK_WIDGET (self->root_box), 32);
  gtk_widget_set_margin_end (GTK_WIDGET (self->root_box), 32);
  gtk_widget_set_margin_top (GTK_WIDGET (self->root_box), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->root_box), 24);

  /* Header */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);

  self->artist_image = GTK_IMAGE (gtk_image_new_from_icon_name ("avatar-default-symbolic"));
  gtk_image_set_pixel_size (self->artist_image, 200);
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_image), "circular");
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_image), "card");
  gtk_box_append (GTK_BOX (header), GTK_WIDGET (self->artist_image));

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign (info, GTK_ALIGN_CENTER);

  GtkWidget *type_label = gtk_label_new ("ARTIST");
  gtk_label_set_xalign (GTK_LABEL (type_label), 0.0);
  gtk_widget_add_css_class (type_label, "caption");
  gtk_widget_add_css_class (type_label, "dim-label");
  gtk_box_append (GTK_BOX (info), type_label);

  self->name_label = GTK_LABEL (gtk_label_new ("Artist"));
  gtk_label_set_xalign (self->name_label, 0.0);
  gtk_label_set_ellipsize (self->name_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (self->name_label), "title-1");
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->name_label));

  self->follow_btn = GTK_BUTTON (gtk_button_new_with_label ("Follow"));
  gtk_widget_add_css_class (GTK_WIDGET (self->follow_btn), "pill");
  g_signal_connect (self->follow_btn, "clicked", G_CALLBACK (on_follow_clicked), self);
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->follow_btn));

  gtk_box_append (GTK_BOX (header), info);
  gtk_box_append (GTK_BOX (self->root_box), header);

  /* Top tracks */
  GtkWidget *top_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *top_title = gtk_label_new ("Popular");
  gtk_label_set_xalign (GTK_LABEL (top_title), 0.0);
  gtk_widget_add_css_class (top_title, "title-3");
  gtk_box_append (GTK_BOX (top_section), top_title);

  self->top_tracks_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->top_tracks_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (GTK_WIDGET (self->top_tracks_list), "boxed-list");
  g_signal_connect (self->top_tracks_list, "row-activated",
                    G_CALLBACK (on_track_activated), self);
  gtk_box_append (GTK_BOX (top_section), GTK_WIDGET (self->top_tracks_list));
  gtk_box_append (GTK_BOX (self->root_box), top_section);

  /* Albums */
  GtkWidget *albums_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *albums_title = gtk_label_new ("Discography");
  gtk_label_set_xalign (GTK_LABEL (albums_title), 0.0);
  gtk_widget_add_css_class (albums_title, "title-3");
  gtk_box_append (GTK_BOX (albums_section), albums_title);

  self->albums_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->albums_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (GTK_WIDGET (self->albums_list), "boxed-list");
  g_signal_connect (self->albums_list, "row-activated",
                    G_CALLBACK (on_album_activated), self);
  gtk_box_append (GTK_BOX (albums_section), GTK_WIDGET (self->albums_list));
  gtk_box_append (GTK_BOX (self->root_box), albums_section);

  /* Related artists */
  GtkWidget *related_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *related_title = gtk_label_new ("Fans Also Like");
  gtk_label_set_xalign (GTK_LABEL (related_title), 0.0);
  gtk_widget_add_css_class (related_title, "title-3");
  gtk_box_append (GTK_BOX (related_section), related_title);

  self->related_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->related_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (GTK_WIDGET (self->related_list), "boxed-list");
  g_signal_connect (self->related_list, "row-activated",
                    G_CALLBACK (on_related_activated), self);
  gtk_box_append (GTK_BOX (related_section), GTK_WIDGET (self->related_list));
  gtk_box_append (GTK_BOX (self->root_box), related_section);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (self->root_box));
  adw_bin_set_child (ADW_BIN (self), scroll);
}

SpotifyGtkArtistPage *
spotifygtk_artist_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_ARTIST_PAGE, NULL);
}

void
spotifygtk_artist_page_set_artist (SpotifyGtkArtistPage *self, JsonObject *artist_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  if (!artist_data) return;

  const gchar *name = json_object_get_string_member_with_default (artist_data, "name", "Artist");
  const gchar *id = json_object_get_string_member_with_default (artist_data, "id", "");

  g_free (self->artist_id);
  self->artist_id = g_strdup (id);

  gtk_label_set_text (self->name_label, name);
}

void
spotifygtk_artist_page_set_top_tracks (SpotifyGtkArtistPage *self, JsonArray *tracks)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));

  clear_list (self->top_tracks_list);
  if (!tracks) return;

  for (guint i = 0; i < json_array_get_length (tracks) && i < 10; i++) {
    JsonObject *track = json_array_get_object_element (tracks, i);
    const gchar *name = json_object_get_string_member_with_default (track, "name", "");
    const gchar *uri = json_object_get_string_member_with_default (track, "uri", "");
    gint64 duration_ms = json_object_has_member (track, "duration_ms") ?
      json_object_get_int_member (track, "duration_ms") : 0;

    g_autofree gchar *dur = duration_str ((gint) duration_ms);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start (box, 8);
    gtk_widget_set_margin_end (box, 8);
    gtk_widget_set_margin_top (box, 6);
    gtk_widget_set_margin_bottom (box, 6);

    GtkWidget *num = gtk_label_new (g_strdup_printf ("%u", i + 1));
    gtk_widget_add_css_class (num, "dim-label");
    gtk_widget_set_size_request (num, 20, -1);
    gtk_box_append (GTK_BOX (box), num);

    GtkWidget *title = gtk_label_new (name);
    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (title, TRUE);
    gtk_box_append (GTK_BOX (box), title);

    GtkWidget *dur_label = gtk_label_new (dur);
    gtk_widget_add_css_class (dur_label, "dim-label");
    gtk_box_append (GTK_BOX (box), dur_label);

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    g_object_set_data_full (G_OBJECT (row), "track-uri", g_strdup (uri), g_free);
    gtk_list_box_append (self->top_tracks_list, row);
  }
}

void
spotifygtk_artist_page_set_albums (SpotifyGtkArtistPage *self, JsonArray *albums)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));

  clear_list (self->albums_list);
  if (!albums) return;

  for (guint i = 0; i < json_array_get_length (albums); i++) {
    JsonObject *album = json_array_get_object_element (albums, i);
    const gchar *name = json_object_get_string_member_with_default (album, "name", "");
    const gchar *id = json_object_get_string_member_with_default (album, "id", "");
    const gchar *year = json_object_get_string_member_with_default (album, "release_date", "");

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start (box, 8);
    gtk_widget_set_margin_end (box, 8);
    gtk_widget_set_margin_top (box, 6);
    gtk_widget_set_margin_bottom (box, 6);

    GtkWidget *icon = gtk_image_new_from_icon_name ("media-optical-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 40);
    gtk_box_append (GTK_BOX (box), icon);

    GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title = gtk_label_new (name);
    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (title, TRUE);
    gtk_box_append (GTK_BOX (info), title);

    GtkWidget *year_label = gtk_label_new (year);
    gtk_widget_add_css_class (year_label, "dim-label");
    gtk_widget_add_css_class (year_label, "caption");
    gtk_box_append (GTK_BOX (info), year_label);

    gtk_box_append (GTK_BOX (box), info);

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    g_object_set_data_full (G_OBJECT (row), "album-id", g_strdup (id), g_free);
    gtk_list_box_append (self->albums_list, row);
  }
}

void
spotifygtk_artist_page_set_related_artists (SpotifyGtkArtistPage *self, JsonArray *artists)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));

  clear_list (self->related_list);
  if (!artists) return;

  for (guint i = 0; i < json_array_get_length (artists); i++) {
    JsonObject *artist = json_array_get_object_element (artists, i);
    const gchar *name = json_object_get_string_member_with_default (artist, "name", "");
    const gchar *id = json_object_get_string_member_with_default (artist, "id", "");

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start (box, 8);
    gtk_widget_set_margin_end (box, 8);
    gtk_widget_set_margin_top (box, 6);
    gtk_widget_set_margin_bottom (box, 6);

    GtkWidget *icon = gtk_image_new_from_icon_name ("avatar-default-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 40);
    gtk_box_append (GTK_BOX (box), icon);

    GtkWidget *title = gtk_label_new (name);
    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_widget_set_hexpand (title, TRUE);
    gtk_box_append (GTK_BOX (box), title);

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    g_object_set_data_full (G_OBJECT (row), "artist-id", g_strdup (id), g_free);
    gtk_list_box_append (self->related_list, row);
  }
}

void
spotifygtk_artist_page_set_following (SpotifyGtkArtistPage *self, gboolean is_following)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  self->is_following = is_following;
  gtk_button_set_label (self->follow_btn, is_following ? "Following" : "Follow");
}

void
spotifygtk_artist_page_set_loading (SpotifyGtkArtistPage *self, gboolean loading)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  /* TODO */
  (void) loading;
}

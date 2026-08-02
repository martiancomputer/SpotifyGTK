/*
 * album_page.c — Album detail view implementation.
 */

#include "album_page.h"
#include "smooth_scroll.h"

struct _SpotifyGtkAlbumPage {
  AdwBin parent_instance;

  GtkBox *root_box;
  GtkImage *cover_image;
  GtkLabel *title_label;
  GtkLabel *artist_label;
  GtkLabel *year_label;
  GtkLabel *track_count_label;
  GtkButton *play_btn;
  GtkButton *shuffle_btn;
  GtkButton *save_btn;
  GtkListBox *track_list;

  gchar *album_id;
  gchar *album_uri;
  gboolean is_saved;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkAlbumPage, spotifygtk_album_page, ADW_TYPE_BIN)

enum {
  PLAY_TRACK,
  PLAY_ALBUM,
  SHUFFLE_ALBUM,
  TOGGLE_SAVED,
  ARTIST_ACTIVATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static gchar *
artist_names_from_array (JsonArray *artists)
{
  if (!artists || json_array_get_length (artists) == 0)
    return g_strdup ("");

  GString *names = g_string_new (NULL);
  for (guint i = 0; i < json_array_get_length (artists); i++) {
    JsonObject *artist = json_array_get_object_element (artists, i);
    const gchar *name = json_object_get_string_member_with_default (artist, "name", "");
    if (*name) {
      if (names->len > 0) g_string_append (names, ", ");
      g_string_append (names, name);
    }
  }
  return g_string_free (names, FALSE);
}

static gchar *
format_duration (gint ms)
{
  gint total_secs = ms / 1000;
  gint mins = total_secs / 60;
  gint secs = total_secs % 60;
  return g_strdup_printf ("%d:%02d", mins, secs);
}

static GtkWidget *
create_track_row (JsonObject *track, guint number)
{
  const gchar *name = json_object_get_string_member_with_default (track, "name", "");
  const gchar *uri = json_object_get_string_member_with_default (track, "uri", "");
  gint64 duration_ms = json_object_has_member (track, "duration_ms") ?
    json_object_get_int_member (track, "duration_ms") : 0;

  g_autofree gchar *duration = format_duration ((gint) duration_ms);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (box, 8);
  gtk_widget_set_margin_end (box, 8);
  gtk_widget_set_margin_top (box, 8);
  gtk_widget_set_margin_bottom (box, 8);

  GtkWidget *num = gtk_label_new (g_strdup_printf ("%u", number));
  gtk_widget_add_css_class (num, "dim-label");
  gtk_widget_set_size_request (num, 24, -1);
  gtk_box_append (GTK_BOX (box), num);

  GtkWidget *title = gtk_label_new (name);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_box_append (GTK_BOX (box), title);

  GtkWidget *dur_label = gtk_label_new (duration);
  gtk_widget_add_css_class (dur_label, "dim-label");
  gtk_widget_add_css_class (dur_label, "numeric");
  gtk_box_append (GTK_BOX (box), dur_label);

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data_full (G_OBJECT (row), "track-uri", g_strdup (uri), g_free);

  return row;
}

static void
on_track_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  SpotifyGtkAlbumPage *self = user_data;
  const gchar *uri = g_object_get_data (G_OBJECT (row), "track-uri");
  if (uri)
    g_signal_emit (self, signals[PLAY_TRACK], 0, uri);
  (void) box;
}

static void
on_play_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkAlbumPage *self = user_data;
  if (self->album_uri)
    g_signal_emit (self, signals[PLAY_ALBUM], 0, self->album_uri);
  (void) button;
}

static void
on_shuffle_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkAlbumPage *self = user_data;
  if (self->album_uri)
    g_signal_emit (self, signals[SHUFFLE_ALBUM], 0, self->album_uri);
  (void) button;
}

static void
on_save_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkAlbumPage *self = user_data;
  self->is_saved = !self->is_saved;
  gtk_button_set_icon_name (self->save_btn,
                            self->is_saved ? "heart-filled-symbolic" : "heart-outline-symbolic");
  if (self->album_id)
    g_signal_emit (self, signals[TOGGLE_SAVED], 0, self->album_id, self->is_saved);
  (void) button;
}

static void
spotifygtk_album_page_dispose (GObject *object)
{
  SpotifyGtkAlbumPage *self = SPOTIFYGTK_ALBUM_PAGE (object);
  g_clear_pointer (&self->album_id, g_free);
  g_clear_pointer (&self->album_uri, g_free);
  G_OBJECT_CLASS (spotifygtk_album_page_parent_class)->dispose (object);
}

static void
spotifygtk_album_page_class_init (SpotifyGtkAlbumPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_album_page_dispose;

  signals[PLAY_TRACK] = g_signal_new ("play-track",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[PLAY_ALBUM] = g_signal_new ("play-album",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SHUFFLE_ALBUM] = g_signal_new ("shuffle-album",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[TOGGLE_SAVED] = g_signal_new ("toggle-saved",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  signals[ARTIST_ACTIVATED] = g_signal_new ("artist-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
spotifygtk_album_page_init (SpotifyGtkAlbumPage *self)
{
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_ORIENTATION_VERTICAL);

  self->root_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 24));
  gtk_widget_set_margin_start (GTK_WIDGET (self->root_box), 32);
  gtk_widget_set_margin_end (GTK_WIDGET (self->root_box), 32);
  gtk_widget_set_margin_top (GTK_WIDGET (self->root_box), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->root_box), 24);

  /* Header */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);

  self->cover_image = GTK_IMAGE (gtk_image_new_from_icon_name ("media-optical-symbolic"));
  gtk_image_set_pixel_size (self->cover_image, 200);
  gtk_widget_add_css_class (GTK_WIDGET (self->cover_image), "card");
  gtk_box_append (GTK_BOX (header), GTK_WIDGET (self->cover_image));

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign (info, GTK_ALIGN_CENTER);

  GtkWidget *type_label = gtk_label_new ("ALBUM");
  gtk_label_set_xalign (GTK_LABEL (type_label), 0.0);
  gtk_widget_add_css_class (type_label, "caption");
  gtk_widget_add_css_class (type_label, "dim-label");
  gtk_box_append (GTK_BOX (info), type_label);

  self->title_label = GTK_LABEL (gtk_label_new ("Album"));
  gtk_label_set_xalign (self->title_label, 0.0);
  gtk_label_set_ellipsize (self->title_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (self->title_label), "title-1");
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->title_label));

  GtkWidget *meta = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->artist_label = GTK_LABEL (gtk_label_new ("Artist"));
  self->year_label = GTK_LABEL (gtk_label_new ("2024"));
  self->track_count_label = GTK_LABEL (gtk_label_new ("0 songs"));

  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "heading");
  gtk_widget_add_css_class (GTK_WIDGET (self->year_label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->track_count_label), "dim-label");

  gtk_box_append (GTK_BOX (meta), GTK_WIDGET (self->artist_label));
  gtk_box_append (GTK_BOX (meta), gtk_label_new ("•"));
  gtk_box_append (GTK_BOX (meta), GTK_WIDGET (self->year_label));
  gtk_box_append (GTK_BOX (meta), gtk_label_new ("•"));
  gtk_box_append (GTK_BOX (meta), GTK_WIDGET (self->track_count_label));
  gtk_box_append (GTK_BOX (info), meta);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->play_btn = GTK_BUTTON (gtk_button_new_with_label ("Play"));
  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "suggested-action");
  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "pill");

  self->shuffle_btn = GTK_BUTTON (gtk_button_new_with_label ("Shuffle"));
  gtk_widget_add_css_class (GTK_WIDGET (self->shuffle_btn), "pill");

  self->save_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("heart-outline-symbolic"));
  gtk_widget_add_css_class (GTK_WIDGET (self->save_btn), "circular");

  g_signal_connect (self->play_btn, "clicked", G_CALLBACK (on_play_clicked), self);
  g_signal_connect (self->shuffle_btn, "clicked", G_CALLBACK (on_shuffle_clicked), self);
  g_signal_connect (self->save_btn, "clicked", G_CALLBACK (on_save_clicked), self);

  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (self->play_btn));
  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (self->shuffle_btn));
  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (self->save_btn));
  gtk_box_append (GTK_BOX (info), buttons);

  gtk_box_append (GTK_BOX (header), info);
  gtk_box_append (GTK_BOX (self->root_box), header);

  /* Track list */
  GtkWidget *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append (GTK_BOX (self->root_box), separator);

  self->track_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->track_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (GTK_WIDGET (self->track_list), "boxed-list");
  g_signal_connect (self->track_list, "row-activated",
                    G_CALLBACK (on_track_activated), self);
  gtk_box_append (GTK_BOX (self->root_box), GTK_WIDGET (self->track_list));

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (self->root_box));
  adw_bin_set_child (ADW_BIN (self), scroll);
}

SpotifyGtkAlbumPage *
spotifygtk_album_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_ALBUM_PAGE, NULL);
}

void
spotifygtk_album_page_set_album (SpotifyGtkAlbumPage *self, JsonObject *album_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_PAGE (self));
  if (!album_data) return;

  const gchar *name = json_object_get_string_member_with_default (album_data, "name", "Album");
  const gchar *id = json_object_get_string_member_with_default (album_data, "id", "");
  const gchar *uri = json_object_get_string_member_with_default (album_data, "uri", "");
  const gchar *release_date = json_object_get_string_member_with_default (album_data, "release_date", "");

  g_free (self->album_id);
  g_free (self->album_uri);
  self->album_id = g_strdup (id);
  self->album_uri = g_strdup (uri);

  gtk_label_set_text (self->title_label, name);
  gtk_label_set_text (self->year_label, release_date ? release_date : "");

  /* Artist names */
  JsonArray *artists = json_object_has_member (album_data, "artists") ?
    json_object_get_array_member (album_data, "artists") : NULL;
  g_autofree gchar *artist_names = artist_names_from_array (artists);
  gtk_label_set_text (self->artist_label, artist_names);

  /* Tracks */
  JsonObject *tracks_obj = json_object_has_member (album_data, "tracks") ?
    json_object_get_object_member (album_data, "tracks") : NULL;
  JsonArray *items = tracks_obj && json_object_has_member (tracks_obj, "items") ?
    json_object_get_array_member (tracks_obj, "items") : NULL;

  /* Clear existing tracks */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->track_list))))
    gtk_list_box_remove (self->track_list, child);

  guint track_count = 0;
  if (items) {
    track_count = json_array_get_length (items);
    for (guint i = 0; i < json_array_get_length (items); i++) {
      JsonObject *track = json_array_get_object_element (items, i);
      gtk_list_box_append (self->track_list, create_track_row (track, i + 1));
    }
  }

  g_autofree gchar *count_text = g_strdup_printf ("%u song%s", track_count,
                                                  track_count == 1 ? "" : "s");
  gtk_label_set_text (self->track_count_label, count_text);
}

void
spotifygtk_album_page_set_loading (SpotifyGtkAlbumPage *self, gboolean loading)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_PAGE (self));
  /* TODO: Show loading state */
  (void) loading;
}

void
spotifygtk_album_page_set_saved (SpotifyGtkAlbumPage *self, gboolean is_saved)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_PAGE (self));
  self->is_saved = is_saved;
  gtk_button_set_icon_name (self->save_btn,
                            is_saved ? "heart-filled-symbolic" : "heart-outline-symbolic");
}

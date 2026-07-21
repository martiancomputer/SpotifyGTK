/*
 * playlist_page.c — Playlist detail view implementation.
 */

#include "playlist_page.h"

struct _SpotifyGtkPlaylistPage {
  AdwBin parent_instance;

  GtkBox *root_box;
  GtkImage *cover_image;
  GtkLabel *name_label;
  GtkLabel *description_label;
  GtkLabel *owner_label;
  GtkLabel *track_count_label;
  GtkButton *play_btn;
  GtkButton *shuffle_btn;
  GtkListBox *track_list;

  gchar *playlist_id;
  gchar *playlist_uri;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkPlaylistPage, spotifygtk_playlist_page, ADW_TYPE_BIN)

enum {
  PLAY_TRACK,
  PLAY_PLAYLIST,
  SHUFFLE_PLAYLIST,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static gchar *
duration_str (gint ms)
{
  gint total_secs = ms / 1000;
  gint mins = total_secs / 60;
  gint secs = total_secs % 60;
  return g_strdup_printf ("%d:%02d", mins, secs);
}

static GtkWidget *
create_track_row (JsonObject *item, guint number)
{
  JsonObject *track = json_object_has_member (item, "track") ?
    json_object_get_object_member (item, "track") : item;

  const gchar *name = json_object_get_string_member_with_default (track, "name", "");
  const gchar *uri = json_object_get_string_member_with_default (track, "uri", "");
  gint64 duration_ms = json_object_has_member (track, "duration_ms") ?
    json_object_get_int_member (track, "duration_ms") : 0;

  /* Artist */
  g_autofree gchar *artist_name = NULL;
  JsonArray *artists = json_object_has_member (track, "artists") ?
    json_object_get_array_member (track, "artists") : NULL;
  if (artists && json_array_get_length (artists) > 0) {
    JsonObject *first = json_array_get_object_element (artists, 0);
    artist_name = g_strdup (json_object_get_string_member_with_default (first, "name", ""));
  }

  /* Album */
  const gchar *album_name = "";
  JsonObject *album = json_object_has_member (track, "album") ?
    json_object_get_object_member (track, "album") : NULL;
  if (album)
    album_name = json_object_get_string_member_with_default (album, "name", "");

  g_autofree gchar *dur = duration_str ((gint) duration_ms);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (box, 8);
  gtk_widget_set_margin_end (box, 8);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);

  GtkWidget *num = gtk_label_new (g_strdup_printf ("%u", number));
  gtk_widget_add_css_class (num, "dim-label");
  gtk_widget_set_size_request (num, 24, -1);
  gtk_box_append (GTK_BOX (box), num);

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (info, TRUE);

  GtkWidget *title = gtk_label_new (name);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (title, "heading");
  gtk_box_append (GTK_BOX (info), title);

  GtkWidget *meta = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  if (artist_name && *artist_name) {
    GtkWidget *artist_label = gtk_label_new (artist_name);
    gtk_widget_add_css_class (artist_label, "dim-label");
    gtk_widget_add_css_class (artist_label, "caption");
    gtk_box_append (GTK_BOX (meta), artist_label);
  }
  if (album_name && *album_name) {
    GtkWidget *sep = gtk_label_new ("•");
    gtk_widget_add_css_class (sep, "dim-label");
    gtk_box_append (GTK_BOX (meta), sep);
    GtkWidget *album_label = gtk_label_new (album_name);
    gtk_widget_add_css_class (album_label, "dim-label");
    gtk_widget_add_css_class (album_label, "caption");
    gtk_box_append (GTK_BOX (meta), album_label);
  }
  gtk_box_append (GTK_BOX (info), meta);

  gtk_box_append (GTK_BOX (box), info);

  GtkWidget *dur_label = gtk_label_new (dur);
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
  SpotifyGtkPlaylistPage *self = user_data;
  const gchar *uri = g_object_get_data (G_OBJECT (row), "track-uri");
  if (uri)
    g_signal_emit (self, signals[PLAY_TRACK], 0, uri);
  (void) box;
}

static void
on_play_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkPlaylistPage *self = user_data;
  if (self->playlist_uri)
    g_signal_emit (self, signals[PLAY_PLAYLIST], 0, self->playlist_uri);
  (void) button;
}

static void
on_shuffle_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkPlaylistPage *self = user_data;
  if (self->playlist_uri)
    g_signal_emit (self, signals[SHUFFLE_PLAYLIST], 0, self->playlist_uri);
  (void) button;
}

static void
spotifygtk_playlist_page_dispose (GObject *object)
{
  SpotifyGtkPlaylistPage *self = SPOTIFYGTK_PLAYLIST_PAGE (object);
  g_clear_pointer (&self->playlist_id, g_free);
  g_clear_pointer (&self->playlist_uri, g_free);
  G_OBJECT_CLASS (spotifygtk_playlist_page_parent_class)->dispose (object);
}

static void
spotifygtk_playlist_page_class_init (SpotifyGtkPlaylistPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_playlist_page_dispose;

  signals[PLAY_TRACK] = g_signal_new ("play-track",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[PLAY_PLAYLIST] = g_signal_new ("play-playlist",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SHUFFLE_PLAYLIST] = g_signal_new ("shuffle-playlist",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clear_track_list (SpotifyGtkPlaylistPage *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->track_list))))
    gtk_list_box_remove (self->track_list, child);
}

static void
spotifygtk_playlist_page_init (SpotifyGtkPlaylistPage *self)
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

  self->cover_image = GTK_IMAGE (gtk_image_new_from_icon_name ("folder-music-symbolic"));
  gtk_image_set_pixel_size (self->cover_image, 200);
  gtk_widget_add_css_class (GTK_WIDGET (self->cover_image), "card");
  gtk_box_append (GTK_BOX (header), GTK_WIDGET (self->cover_image));

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign (info, GTK_ALIGN_CENTER);

  GtkWidget *type_label = gtk_label_new ("PLAYLIST");
  gtk_label_set_xalign (GTK_LABEL (type_label), 0.0);
  gtk_widget_add_css_class (type_label, "caption");
  gtk_widget_add_css_class (type_label, "dim-label");
  gtk_box_append (GTK_BOX (info), type_label);

  self->name_label = GTK_LABEL (gtk_label_new ("Playlist"));
  gtk_label_set_xalign (self->name_label, 0.0);
  gtk_label_set_ellipsize (self->name_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (self->name_label), "title-1");
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->name_label));

  self->description_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (self->description_label, 0.0);
  gtk_label_set_ellipsize (self->description_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_wrap (self->description_label, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->description_label), "dim-label");
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->description_label));

  GtkWidget *meta = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->owner_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->owner_label), "heading");
  gtk_box_append (GTK_BOX (meta), GTK_WIDGET (self->owner_label));

  self->track_count_label = GTK_LABEL (gtk_label_new ("0 songs"));
  gtk_widget_add_css_class (GTK_WIDGET (self->track_count_label), "dim-label");
  gtk_box_append (GTK_BOX (meta), GTK_WIDGET (self->track_count_label));
  gtk_box_append (GTK_BOX (info), meta);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->play_btn = GTK_BUTTON (gtk_button_new_with_label ("Play"));
  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "suggested-action");
  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "pill");

  self->shuffle_btn = GTK_BUTTON (gtk_button_new_with_label ("Shuffle"));
  gtk_widget_add_css_class (GTK_WIDGET (self->shuffle_btn), "pill");

  g_signal_connect (self->play_btn, "clicked", G_CALLBACK (on_play_clicked), self);
  g_signal_connect (self->shuffle_btn, "clicked", G_CALLBACK (on_shuffle_clicked), self);

  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (self->play_btn));
  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (self->shuffle_btn));
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

SpotifyGtkPlaylistPage *
spotifygtk_playlist_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_PLAYLIST_PAGE, NULL);
}

void
spotifygtk_playlist_page_set_playlist (SpotifyGtkPlaylistPage *self, JsonObject *playlist_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYLIST_PAGE (self));
  if (!playlist_data) return;

  const gchar *name = json_object_get_string_member_with_default (playlist_data, "name", "Playlist");
  const gchar *id = json_object_get_string_member_with_default (playlist_data, "id", "");
  const gchar *uri = json_object_get_string_member_with_default (playlist_data, "uri", "");
  const gchar *description = json_object_get_string_member_with_default (playlist_data, "description", "");

  g_free (self->playlist_id);
  g_free (self->playlist_uri);
  self->playlist_id = g_strdup (id);
  self->playlist_uri = g_strdup (uri);

  gtk_label_set_text (self->name_label, name);
  gtk_label_set_text (self->description_label, description ? description : "");

  /* Owner */
  JsonObject *owner = json_object_has_member (playlist_data, "owner") ?
    json_object_get_object_member (playlist_data, "owner") : NULL;
  const gchar *owner_name = owner ?
    json_object_get_string_member_with_default (owner, "display_name", "") : "";
  gtk_label_set_text (self->owner_label, owner_name);

  /* Track count */
  JsonObject *tracks = json_object_has_member (playlist_data, "tracks") ?
    json_object_get_object_member (playlist_data, "tracks") : NULL;
  gint64 total = tracks && json_object_has_member (tracks, "total") ?
    json_object_get_int_member (tracks, "total") : 0;

  g_autofree gchar *count_text = g_strdup_printf ("%" G_GINT64_FORMAT " song%s",
                                                   total, total == 1 ? "" : "s");
  gtk_label_set_text (self->track_count_label, count_text);
}

void
spotifygtk_playlist_page_set_tracks (SpotifyGtkPlaylistPage *self, JsonArray *tracks)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYLIST_PAGE (self));

  clear_track_list (self);
  if (!tracks) return;

  for (guint i = 0; i < json_array_get_length (tracks); i++) {
    JsonObject *item = json_array_get_object_element (tracks, i);
    gtk_list_box_append (self->track_list, create_track_row (item, i + 1));
  }
}

void
spotifygtk_playlist_page_append_tracks (SpotifyGtkPlaylistPage *self, JsonArray *tracks)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYLIST_PAGE (self));
  if (!tracks) return;

  /* Get current count */
  guint offset = 0;
  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->track_list));
  while (child) { offset++; child = gtk_widget_get_next_sibling (child); }

  for (guint i = 0; i < json_array_get_length (tracks); i++) {
    JsonObject *item = json_array_get_object_element (tracks, i);
    gtk_list_box_append (self->track_list, create_track_row (item, offset + i + 1));
  }
}

void
spotifygtk_playlist_page_set_loading (SpotifyGtkPlaylistPage *self, gboolean loading)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYLIST_PAGE (self));
  /* TODO */
  (void) loading;
}

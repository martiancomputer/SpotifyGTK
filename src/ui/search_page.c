#include "config.h"
#include "search_page.h"

struct _SpotifySearchPage {
  AdwBin      parent_instance;
  GtkEntry   *search_entry;
  GtkListBox *results_list;
  SpotifyApi *api;   /* borrowed reference — owned by SpotifyGtkApp, outlives this widget */
};

G_DEFINE_FINAL_TYPE (SpotifySearchPage, spotifygtk_search_page, ADW_TYPE_BIN)

static void
clear_results (SpotifySearchPage *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->results_list))))
    gtk_list_box_remove (self->results_list, child);
}

static GtkWidget *
build_track_row (JsonObject *track)
{
  const gchar *name = json_object_get_string_member_with_default (track, "name", "");
  const gchar *uri  = json_object_get_string_member_with_default (track, "uri", "");

  g_autofree gchar *artist_names = NULL;
  JsonArray *artists = json_object_has_member (track, "artists")
    ? json_object_get_array_member (track, "artists") : NULL;
  if (artists && json_array_get_length (artists) > 0) {
    JsonObject *first = json_array_get_object_element (artists, 0);
    artist_names = g_strdup (json_object_get_string_member_with_default (first, "name", ""));
  }

  GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start  (row_box, 8);
  gtk_widget_set_margin_end    (row_box, 8);
  gtk_widget_set_margin_top    (row_box, 6);
  gtk_widget_set_margin_bottom (row_box, 6);

  GtkWidget *icon = gtk_image_new_from_icon_name ("audio-x-generic-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 32);

  GtkWidget *vbox        = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *name_label   = gtk_label_new (name);
  GtkWidget *artist_label = gtk_label_new (artist_names ? artist_names : "");
  gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
  gtk_label_set_xalign (GTK_LABEL (artist_label), 0.0);
  gtk_widget_add_css_class (artist_label, "dim-label");
  gtk_widget_add_css_class (artist_label, "caption");

  gtk_box_append (GTK_BOX (vbox), name_label);
  gtk_box_append (GTK_BOX (vbox), artist_label);
  gtk_box_append (GTK_BOX (row_box), icon);
  gtk_box_append (GTK_BOX (row_box), vbox);

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), row_box);
  g_object_set_data_full (G_OBJECT (row), "track-uri", g_strdup (uri), g_free);

  return row;
}

static void
on_search_results (SpotifyApi *api, JsonObject *result, GError *error, gpointer user_data)
{
  SpotifySearchPage *self = SPOTIFYGTK_SEARCH_PAGE (user_data);
  (void) api;

  clear_results (self);

  if (error) {
    g_warning ("Search request failed: %s", error->message);
    return;
  }
  if (!result) return;

  if (json_object_has_member (result, "error")) {
    JsonObject *err_obj = json_object_get_object_member (result, "error");
    g_warning ("Spotify API error: %s",
              json_object_get_string_member_with_default (err_obj, "message", "unknown"));
    return;
  }

  if (!json_object_has_member (result, "tracks")) return;
  JsonObject *tracks_obj = json_object_get_object_member (result, "tracks");
  if (!json_object_has_member (tracks_obj, "items")) return;
  JsonArray *items = json_object_get_array_member (tracks_obj, "items");

  guint n = json_array_get_length (items);
  for (guint i = 0; i < n; i++) {
    JsonObject *track = json_array_get_object_element (items, i);
    gtk_list_box_append (self->results_list, build_track_row (track));
  }
}

static void
on_search_activate (GtkEntry *entry, gpointer user_data)
{
  SpotifySearchPage *self = SPOTIFYGTK_SEARCH_PAGE (user_data);

  if (!self->api) {
    g_warning ("Search attempted before API client was ready");
    return;
  }

  const gchar *query = gtk_editable_get_text (GTK_EDITABLE (entry));
  if (!query || *query == '\0') return;

  spotifygtk_api_search (self->api, query, "track", on_search_results, self);
}

static void
on_play_result (SpotifyApi *api, JsonObject *result, GError *error, gpointer user_data)
{
  (void) api; (void) result; (void) user_data;
  if (error)
    g_warning ("Playback request failed: %s (do you have an active Spotify device open?)",
              error->message);
}

static void
on_result_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  SpotifySearchPage *self = SPOTIFYGTK_SEARCH_PAGE (user_data);
  (void) box;

  if (!self->api) return;

  const gchar *uri = g_object_get_data (G_OBJECT (row), "track-uri");
  if (!uri || *uri == '\0') return;

  spotifygtk_api_play_track (self->api, uri, on_play_result, NULL);
}

static void
spotifygtk_search_page_constructed (GObject *object)
{
  SpotifySearchPage *self = SPOTIFYGTK_SEARCH_PAGE (object);
  G_OBJECT_CLASS (spotifygtk_search_page_parent_class)->constructed (object);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start  (box, 24);
  gtk_widget_set_margin_end    (box, 24);
  gtk_widget_set_margin_top    (box, 24);
  gtk_widget_set_margin_bottom (box, 24);

  self->search_entry = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_placeholder_text (self->search_entry, "Search songs, artists, albums...");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->search_entry));

  self->results_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->results_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (GTK_WIDGET (self->results_list), "boxed-list");

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (self->results_list));
  gtk_widget_set_vexpand (scroll, TRUE);

  gtk_box_append (GTK_BOX (box), scroll);
  adw_bin_set_child (ADW_BIN (self), box);

  g_signal_connect (self->search_entry, "activate", G_CALLBACK (on_search_activate), self);
  g_signal_connect (self->results_list, "row-activated", G_CALLBACK (on_result_row_activated), self);
}

void
spotifygtk_search_page_set_api (SpotifySearchPage *self, SpotifyApi *api)
{
  g_return_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self));
  self->api = api;
}

static void spotifygtk_search_page_class_init (SpotifySearchPageClass *klass)
{
  G_OBJECT_CLASS (klass)->constructed = spotifygtk_search_page_constructed;
}

static void spotifygtk_search_page_init (SpotifySearchPage *self) { (void) self; }

SpotifySearchPage *
spotifygtk_search_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_SEARCH_PAGE, NULL);
}
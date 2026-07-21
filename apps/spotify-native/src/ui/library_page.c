/*
 * library_page.c — Library page implementation.
 */

#include "library_page.h"

struct _SpotifyGtkLibraryPage {
  GtkBox parent_instance;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkLibraryPage, spotifygtk_library_page, GTK_TYPE_BOX)

static void
spotifygtk_library_page_class_init (SpotifyGtkLibraryPageClass *klass)
{
  (void) klass;
}

static void
spotifygtk_library_page_init (SpotifyGtkLibraryPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 24);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  GtkWidget *title = gtk_label_new ("Library");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (self), title);

  GtkWidget *note = gtk_label_new (
    "Your playlists need spclient's rootlist endpoint, which returns "
    "playlist4_external protobuf — a much larger schema than the track "
    "metadata the rest of the catalog uses, and the one remaining piece "
    "of this migration.\n\n"
    "Playlist contents already work once a playlist URI is known: the "
    "context-resolve path handles spotify:playlist:<id> the same way it "
    "handles search and liked songs.");
  gtk_widget_add_css_class (note, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (note), 0.0);
  gtk_label_set_wrap (GTK_LABEL (note), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (note), 70);
  gtk_box_append (GTK_BOX (self), note);
}

SpotifyGtkLibraryPage *
spotifygtk_library_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_LIBRARY_PAGE, NULL);
}

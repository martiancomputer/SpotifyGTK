#include "config.h"
#include "search_page.h"

struct _SpotifySearchPage {
  AdwBin      parent_instance;
  GtkEntry   *search_entry;
  GtkListBox *results_list;
};

G_DEFINE_FINAL_TYPE (SpotifySearchPage, spotifygtk_search_page, ADW_TYPE_BIN)

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

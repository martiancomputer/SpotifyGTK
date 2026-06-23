#include "config.h"
#include "window.h"
#include "now_playing.h"
#include "search_page.h"

struct _SpotifyWindow {
  AdwApplicationWindow  parent_instance;

  AdwNavigationSplitView *split_view;
  AdwToolbarView         *content_view;
  GtkStack               *page_stack;

  SpotifyNowPlayingBar   *now_playing_bar;
  SpotifySearchPage      *search_page;

  gboolean                authenticated;
};

G_DEFINE_FINAL_TYPE (SpotifyWindow, spotifygtk_window, ADW_TYPE_APPLICATION_WINDOW)

static const struct { const gchar *icon; const gchar *label; const gchar *page; } SIDEBAR[] = {
  { "music-note-symbolic",    "Now Playing", "now-playing" },
  { "system-search-symbolic", "Search",      "search"      },
  { "view-list-symbolic",     "Library",     "library"     },
};

static void
on_sidebar_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  SpotifyWindow *self = SPOTIFYGTK_WINDOW (user_data);
  gint idx = gtk_list_box_row_get_index (row);
  if (idx >= 0 && (gsize) idx < G_N_ELEMENTS (SIDEBAR))
    gtk_stack_set_visible_child_name (self->page_stack, SIDEBAR[idx].page);
}

static GtkWidget *
build_sidebar (SpotifyWindow *self)
{
  GtkWidget *box      = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list_box), GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (list_box, "navigation-sidebar");

  for (gsize i = 0; i < G_N_ELEMENTS (SIDEBAR); i++) {
    GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start  (row_box, 12);
    gtk_widget_set_margin_end    (row_box, 12);
    gtk_widget_set_margin_top    (row_box, 8);
    gtk_widget_set_margin_bottom (row_box, 8);

    GtkWidget *icon  = gtk_image_new_from_icon_name (SIDEBAR[i].icon);
    GtkWidget *label = gtk_label_new (SIDEBAR[i].label);
    gtk_box_append (GTK_BOX (row_box), icon);
    gtk_box_append (GTK_BOX (row_box), label);

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), row_box);
    gtk_list_box_append (GTK_LIST_BOX (list_box), row);
  }

  g_signal_connect (list_box, "row-activated", G_CALLBACK (on_sidebar_row_activated), self);
  gtk_box_append (GTK_BOX (box), list_box);

  GtkListBoxRow *first = gtk_list_box_get_row_at_index (GTK_LIST_BOX (list_box), 0);
  if (first) gtk_list_box_select_row (GTK_LIST_BOX (list_box), first);

  return box;
}

static void
spotifygtk_window_constructed (GObject *object)
{
  SpotifyWindow *self = SPOTIFYGTK_WINDOW (object);
  G_OBJECT_CLASS (spotifygtk_window_parent_class)->constructed (object);

  gtk_window_set_title          (GTK_WINDOW (self), "SpotifyGTK");
  gtk_window_set_default_size   (GTK_WINDOW (self), 1100, 700);
  gtk_window_set_icon_name      (GTK_WINDOW (self), APP_ID);

  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());

  GtkWidget *sidebar_content = build_sidebar (self);
  AdwNavigationPage *sidebar_page = ADW_NAVIGATION_PAGE (
    adw_navigation_page_new (sidebar_content, "SpotifyGTK"));

  self->page_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->page_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);

  GtkWidget *np_placeholder = gtk_label_new ("Now Playing");
  gtk_stack_add_named (self->page_stack, np_placeholder, "now-playing");

  self->search_page = spotifygtk_search_page_new ();
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->search_page), "search");

  GtkWidget *lib_placeholder = gtk_label_new ("Library (coming soon)");
  gtk_stack_add_named (self->page_stack, lib_placeholder, "library");

  self->content_view = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar    (self->content_view, GTK_WIDGET (header));
  adw_toolbar_view_set_content    (self->content_view, GTK_WIDGET (self->page_stack));

  self->now_playing_bar = spotifygtk_now_playing_bar_new ();
  adw_toolbar_view_add_bottom_bar (self->content_view, GTK_WIDGET (self->now_playing_bar));

  self->split_view = ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
  adw_navigation_split_view_set_sidebar (self->split_view, sidebar_page);
  adw_navigation_split_view_set_content (self->split_view,
    ADW_NAVIGATION_PAGE (adw_navigation_page_new (GTK_WIDGET (self->content_view), "Content")));

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), GTK_WIDGET (self->split_view));
}

void
spotifygtk_window_set_authenticated (SpotifyWindow *self, gboolean authenticated)
{
  g_return_if_fail (SPOTIFYGTK_IS_WINDOW (self));
  self->authenticated = authenticated;
}

static void spotifygtk_window_class_init (SpotifyWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->constructed = spotifygtk_window_constructed;
}

static void spotifygtk_window_init (SpotifyWindow *self) { (void) self; }

SpotifyWindow *
spotifygtk_window_new (GtkApplication *app)
{
  return g_object_new (SPOTIFYGTK_TYPE_WINDOW, "application", app, NULL);
}

/*
 * sidebar.c — Navigation sidebar implementation.
 */

#include "sidebar.h"

struct _SpotifyGtkSidebar {
  GtkBox parent_instance;
  GtkListBox *nav_list;
  GtkListBox *pinned_list;
  gchar *selected_page;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkSidebar, spotifygtk_sidebar, GTK_TYPE_BOX)

enum {
  PAGE_ACTIVATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Navigation items matching mockup.
 *
 * The mockup also drew a "Downloads" entry; it is deliberately absent. This
 * client is playback-only by design (README, principle 3), so there is no
 * offline store for it to point at. */
static const struct { const gchar *id; const gchar *label; } NAV_ITEMS[] = {
  { "home",    "Home" },
  { "search",  "Search" },
  { "liked",   "Liked Songs" },
  { "library", "Library" },
};

static void
on_nav_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  SpotifyGtkSidebar *self = user_data;
  gint idx = gtk_list_box_row_get_index (row);
  if (idx >= 0 && (guint) idx < G_N_ELEMENTS (NAV_ITEMS)) {
    g_free (self->selected_page);
    self->selected_page = g_strdup (NAV_ITEMS[idx].id);
    g_signal_emit (self, signals[PAGE_ACTIVATED], 0, NAV_ITEMS[idx].id);
  }
  (void) box;
}

static void
spotifygtk_sidebar_dispose (GObject *object)
{
  SpotifyGtkSidebar *self = SPOTIFYGTK_SIDEBAR (object);
  g_clear_pointer (&self->selected_page, g_free);
  G_OBJECT_CLASS (spotifygtk_sidebar_parent_class)->dispose (object);
}

static void
spotifygtk_sidebar_class_init (SpotifyGtkSidebarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_sidebar_dispose;

  signals[PAGE_ACTIVATED] = g_signal_new ("page-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
spotifygtk_sidebar_init (SpotifyGtkSidebar *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "sidebar");
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* Navigation list */
  self->nav_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->nav_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (GTK_WIDGET (self->nav_list), "navigation-sidebar");

  for (guint i = 0; i < G_N_ELEMENTS (NAV_ITEMS); i++) {
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *label = gtk_label_new (NAV_ITEMS[i].label);
    gtk_widget_add_css_class (label, "normal-text");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_widget_set_margin_start (label, 16);
    gtk_widget_set_margin_end (label, 16);
    gtk_widget_set_margin_top (label, 12);
    gtk_widget_set_margin_bottom (label, 12);
    gtk_widget_add_css_class (row, "sidebar-item");
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
    gtk_list_box_append (self->nav_list, row);
  }

  g_signal_connect (self->nav_list, "row-activated",
                    G_CALLBACK (on_nav_row_activated), self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->nav_list));

  /* Divider */
  GtkWidget *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top (separator, 12);
  gtk_widget_set_margin_bottom (separator, 12);
  gtk_widget_set_margin_start (separator, 16);
  gtk_widget_set_margin_end (separator, 16);
  gtk_box_append (GTK_BOX (self), separator);

  /* Pinned section label */
  GtkWidget *pinned_label = gtk_label_new ("Pinned");
  gtk_widget_add_css_class (pinned_label, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (pinned_label), 0.0);
  gtk_widget_set_margin_start (pinned_label, 16);
  gtk_widget_set_margin_bottom (pinned_label, 8);
  gtk_box_append (GTK_BOX (self), pinned_label);

  /* Pinned list */
  self->pinned_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->pinned_list, GTK_SELECTION_NONE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->pinned_list));

  /* Pin hint */
  GtkWidget *hint = gtk_label_new ("+ Pin current album / playlist");
  gtk_widget_add_css_class (hint, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_widget_set_margin_start (hint, 16);
  gtk_widget_set_margin_top (hint, 8);
  gtk_box_append (GTK_BOX (self), hint);

  /* Select Home by default */
  gtk_list_box_select_row (self->nav_list,
                           gtk_list_box_get_row_at_index (self->nav_list, 0));
}

SpotifyGtkSidebar *
spotifygtk_sidebar_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_SIDEBAR, NULL);
}

void
spotifygtk_sidebar_add_pinned (SpotifyGtkSidebar *self,
                               const gchar *id,
                               const gchar *name,
                               const gchar *type)
{
  g_return_if_fail (SPOTIFYGTK_IS_SIDEBAR (self));

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_widget_add_css_class (row, "pinned-card");

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_start (box, 8);
  gtk_widget_set_margin_end (box, 8);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);

  GtkWidget *label = gtk_label_new (name);
  gtk_widget_add_css_class (label, "normal-text");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), label);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  gtk_list_box_append (self->pinned_list, row);

  (void) id; (void) type;
}

void
spotifygtk_sidebar_clear_pinned (SpotifyGtkSidebar *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_SIDEBAR (self));

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->pinned_list))))
    gtk_list_box_remove (self->pinned_list, child);
}

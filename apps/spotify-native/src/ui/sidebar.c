/*
 * sidebar.c — Navigation sidebar implementation.
 *
 * Per the reference design: icon + label nav rows with a rounded pill for
 * the selected item, a divider, then a "Pinned" section whose rows carry
 * artwork, a name, a type line, and a pin marker. A pin action and a
 * collapse action sit at the bottom.
 */

#include "sidebar.h"

struct _SpotifyGtkSidebar {
  GtkBox parent_instance;
  GtkListBox *nav_list;
  GtkListBox *pinned_list;
  GtkLabel   *pinned_heading;
  gchar *selected_page;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkSidebar, spotifygtk_sidebar, GTK_TYPE_BOX)

enum {
  PAGE_ACTIVATED,
  PINNED_ACTIVATED,
  COLLAPSE_TOGGLED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Navigation items matching the reference.
 *
 * "Downloads" is deliberately absent: this client is playback-only by
 * design (README principle 3), so there is no offline store behind it and
 * a nav entry would lead to a page that cannot exist. */
static const struct {
  const gchar *id;
  const gchar *label;
  const gchar *icon;
} NAV_ITEMS[] = {
  { "home",    "Home",        "go-home-symbolic" },
  { "search",  "Search",      "system-search-symbolic" },
  { "liked",   "Liked Songs", "emblem-favorite-symbolic" },
  { "library", "Library",     "view-list-symbolic" },
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
on_pinned_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  SpotifyGtkSidebar *self = user_data;
  const gchar *id = g_object_get_data (G_OBJECT (row), "pinned-id");

  if (id && *id)
    g_signal_emit (self, signals[PINNED_ACTIVATED], 0, id);
  (void) box;
}

static void
on_collapse_clicked (GtkButton *button, gpointer user_data)
{
  g_signal_emit (user_data, signals[COLLAPSE_TOGGLED], 0);
  (void) button;
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

  signals[PINNED_ACTIVATED] = g_signal_new ("pinned-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[COLLAPSE_TOGGLED] = g_signal_new ("collapse-toggled",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static GtkWidget *
build_nav_row (const gchar *label_text, const gchar *icon_name)
{
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_widget_add_css_class (row, "sidebar-item");

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_widget_set_margin_start (box, 14);
  gtk_widget_set_margin_end (box, 14);
  gtk_widget_set_margin_top (box, 10);
  gtk_widget_set_margin_bottom (box, 10);

  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 18);
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *label = gtk_label_new (label_text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), label);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

static GtkWidget *
build_sidebar_action (const gchar *label_text, const gchar *icon_name)
{
  GtkWidget *button = gtk_button_new ();
  gtk_widget_add_css_class (button, "flat");

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 14);
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *label = gtk_label_new (label_text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_add_css_class (label, "sidebar-action");
  gtk_box_append (GTK_BOX (box), label);

  gtk_button_set_child (GTK_BUTTON (button), box);
  return button;
}

static void
spotifygtk_sidebar_init (SpotifyGtkSidebar *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "sidebar");
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* --- Navigation --- */
  self->nav_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->nav_list, GTK_SELECTION_SINGLE);
  gtk_widget_set_margin_top (GTK_WIDGET (self->nav_list), 10);

  for (guint i = 0; i < G_N_ELEMENTS (NAV_ITEMS); i++)
    gtk_list_box_append (self->nav_list,
                         build_nav_row (NAV_ITEMS[i].label, NAV_ITEMS[i].icon));

  g_signal_connect (self->nav_list, "row-activated",
                    G_CALLBACK (on_nav_row_activated), self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->nav_list));

  /* Home is the startup page, so it starts selected. */
  GtkListBoxRow *first = gtk_list_box_get_row_at_index (self->nav_list, 0);
  if (first)
    gtk_list_box_select_row (self->nav_list, first);

  /* --- Divider --- */
  GtkWidget *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top (separator, 14);
  gtk_widget_set_margin_bottom (separator, 12);
  gtk_widget_set_margin_start (separator, 20);
  gtk_widget_set_margin_end (separator, 20);
  gtk_box_append (GTK_BOX (self), separator);

  /* --- Pinned --- */
  self->pinned_heading = GTK_LABEL (gtk_label_new ("Pinned"));
  gtk_widget_add_css_class (GTK_WIDGET (self->pinned_heading), "sidebar-heading");
  gtk_label_set_xalign (self->pinned_heading, 0.0);
  gtk_widget_set_margin_start (GTK_WIDGET (self->pinned_heading), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->pinned_heading), 8);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->pinned_heading));

  GtkWidget *pinned_scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (pinned_scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (pinned_scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  self->pinned_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->pinned_list, GTK_SELECTION_NONE);
  g_signal_connect (self->pinned_list, "row-activated",
                    G_CALLBACK (on_pinned_row_activated), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (pinned_scroller),
                                 GTK_WIDGET (self->pinned_list));
  gtk_box_append (GTK_BOX (self), pinned_scroller);

  /* --- Bottom actions --- */
  GtkWidget *pin_action = build_sidebar_action ("Pin current album or playlist",
                                                "list-add-symbolic");
  gtk_widget_set_margin_start (pin_action, 10);
  gtk_widget_set_margin_end (pin_action, 10);
  /* Pinning needs somewhere to persist to, and there is no settings store
   * yet — disabled rather than silently doing nothing. */
  gtk_widget_set_sensitive (pin_action, FALSE);
  gtk_widget_set_tooltip_text (pin_action, "Pinning isn’t implemented yet");
  gtk_box_append (GTK_BOX (self), pin_action);

  GtkWidget *collapse_action = build_sidebar_action ("Collapse", "go-previous-symbolic");
  gtk_widget_set_margin_start (collapse_action, 10);
  gtk_widget_set_margin_end (collapse_action, 10);
  gtk_widget_set_margin_bottom (collapse_action, 16);
  g_signal_connect (collapse_action, "clicked", G_CALLBACK (on_collapse_clicked), self);
  gtk_box_append (GTK_BOX (self), collapse_action);
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

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (box, 14);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);

  /* Artwork placeholder. The image cache still lives in spotify-connect,
   * so there is nothing to decode covers with here yet. */
  GtkWidget *art = gtk_image_new_from_icon_name ("media-optical-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (art), 22);
  gtk_widget_set_size_request (art, 42, 42);
  gtk_widget_add_css_class (art, "art-thumb");
  gtk_box_append (GTK_BOX (box), art);

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_set_valign (info, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (info, TRUE);

  GtkWidget *name_label = gtk_label_new (name ? name : "Untitled");
  gtk_widget_add_css_class (name_label, "normal-text");
  gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (info), name_label);

  GtkWidget *type_label = gtk_label_new (type ? type : "");
  gtk_widget_add_css_class (type_label, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (type_label), 0.0);
  gtk_box_append (GTK_BOX (info), type_label);

  gtk_box_append (GTK_BOX (box), info);

  GtkWidget *pin = gtk_image_new_from_icon_name ("view-pin-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (pin), 14);
  gtk_widget_add_css_class (pin, "pin-icon");
  gtk_widget_set_valign (pin, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), pin);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data_full (G_OBJECT (row), "pinned-id", g_strdup (id), g_free);

  gtk_list_box_append (self->pinned_list, row);
}

void
spotifygtk_sidebar_clear_pinned (SpotifyGtkSidebar *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_SIDEBAR (self));

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->pinned_list))))
    gtk_list_box_remove (self->pinned_list, child);
}

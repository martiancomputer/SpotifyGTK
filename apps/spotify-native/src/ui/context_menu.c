/*
 * context_menu.c — see context_menu.h.
 */

#include "context_menu.h"

#define MENU_MIN_WIDTH 200

struct _SpotifyGtkContextMenu {
  GtkWidget *box;
};

SpotifyGtkContextMenu *
spotifygtk_context_menu_new (void)
{
  SpotifyGtkContextMenu *menu = g_new0 (SpotifyGtkContextMenu, 1);
  menu->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_size_request (menu->box, MENU_MIN_WIDTH, -1);
  return menu;
}

void
spotifygtk_context_menu_add (SpotifyGtkContextMenu *menu, const gchar *label,
                             gboolean enabled, const gchar *tooltip,
                             GCallback cb, gpointer cb_data)
{
  g_return_if_fail (menu != NULL && label != NULL);

  GtkWidget *button = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (button, "flat");
  gtk_button_set_has_frame (GTK_BUTTON (button), FALSE);

  /* Left-align the label: a centred one reads as a dialog button rather than a
   * menu entry. */
  GtkWidget *child = gtk_button_get_child (GTK_BUTTON (button));
  if (GTK_IS_LABEL (child))
    gtk_label_set_xalign (GTK_LABEL (child), 0.0);

  gtk_widget_set_sensitive (button, enabled);
  if (tooltip)
    gtk_widget_set_tooltip_text (button, tooltip);
  if (enabled && cb)
    g_signal_connect (button, "clicked", cb, cb_data);

  gtk_box_append (GTK_BOX (menu->box), button);
}

void
spotifygtk_context_menu_add_separator (SpotifyGtkContextMenu *menu)
{
  g_return_if_fail (menu != NULL);
  gtk_box_append (GTK_BOX (menu->box),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
}

/*
 * Drop a dismissed popover once GTK has finished with it. See the connect site
 * for why this cannot happen inside the "closed" handler.
 */
static gboolean
menu_unparent_idle (gpointer data)
{
  GtkWidget *popover = data;
  if (GTK_IS_WIDGET (popover))
    gtk_widget_unparent (popover);
  g_object_unref (popover);
  return G_SOURCE_REMOVE;
}

/* The widget the menu hangs off is going away; take the menu with it. */
static void
menu_anchor_destroyed (GtkWidget *anchor, gpointer user_data)
{
  GtkWidget *popover = user_data;
  (void) anchor;
  if (GTK_IS_WIDGET (popover) && gtk_widget_get_parent (popover))
    gtk_widget_unparent (popover);
}

static void
menu_close_deferred (GtkPopover *popover, gpointer user_data)
{
  (void) user_data;
  g_idle_add (menu_unparent_idle, g_object_ref (popover));
}

void
spotifygtk_context_menu_present (SpotifyGtkContextMenu *menu, GtkWidget *anchor,
                                 gdouble x, gdouble y,
                                 gpointer ctx, GDestroyNotify ctx_free)
{
  g_return_if_fail (menu != NULL && GTK_IS_WIDGET (anchor));

  GtkWidget *popover = gtk_popover_new ();
  gtk_widget_add_css_class (popover, "menu");
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_widget_set_halign (popover, GTK_ALIGN_START);
  gtk_popover_set_child (GTK_POPOVER (popover), menu->box);
  gtk_widget_set_parent (popover, anchor);
  gtk_popover_set_pointing_to (GTK_POPOVER (popover),
                               &(GdkRectangle){ (int) x, (int) y, 1, 1 });

  if (ctx)
    g_object_set_data_full (G_OBJECT (popover), "menu-ctx", ctx, ctx_free);

  /*
   * Unparent on dismissal so the popover and its context are freed -- without
   * it the menu leaks and outlives the row it was anchored to.
   *
   * Deferred to an idle rather than done in the handler. "closed" is emitted
   * from inside gtk_widget_hide(), so unparenting there destroys the widget
   * while GTK is still walking it: the renderer is unrealized underneath the
   * hide that is still in progress, and it aborts in malloc with heap
   * corruption. Two cores showed exactly that chain -- notify -> hide ->
   * closed -> unparent -> unrealize -> abort.
   *
   * By the time the idle runs the emission has unwound and the widget is
   * merely hidden, which is a safe moment to take it apart.
   */
  g_signal_connect (popover, "closed", G_CALLBACK (menu_close_deferred), NULL);

  /*
   * The anchor can go first.
   *
   * A card is recycled or a whole grid is replaced while its menu is still
   * open, and then the anchor is finalised with the popover still parented to
   * it -- GTK says so ("Finalizing GtkButton, but it still has children left:
   * GtkPopover") and the popover leaks along with its context. Unparenting
   * here is safe in a way doing it from "closed" is not: nothing is mid-hide.
   */
  g_signal_connect (anchor, "destroy", G_CALLBACK (menu_anchor_destroyed), popover);

  gtk_popover_popup (GTK_POPOVER (popover));
  g_free (menu);
}

GtkPopover *
spotifygtk_context_menu_get_popover (GtkWidget *entry_button)
{
  GtkWidget *p = gtk_widget_get_ancestor (entry_button, GTK_TYPE_POPOVER);
  return p ? GTK_POPOVER (p) : NULL;
}

gpointer
spotifygtk_context_menu_get_context (GtkWidget *entry_button)
{
  GtkPopover *popover = spotifygtk_context_menu_get_popover (entry_button);
  return popover ? g_object_get_data (G_OBJECT (popover), "menu-ctx") : NULL;
}

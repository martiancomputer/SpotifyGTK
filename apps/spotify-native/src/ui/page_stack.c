#include "page_stack.h"

void
spotifygtk_page_stack_show (GtkStack *stack, const gchar *name)
{
  GtkWidget *page = gtk_stack_get_child_by_name (stack, name);
  g_return_if_fail (page != NULL);
  gtk_widget_set_visible (page, TRUE);
  gtk_stack_set_visible_child (stack, page);
}

void
spotifygtk_page_stack_hide_inactive (GtkStack *stack)
{
  /* The outgoing page still supplies the crossfade until it completes. */
  if (gtk_stack_get_transition_running (stack))
    return;

  GtkWidget *active = gtk_stack_get_visible_child (stack);
  for (GtkWidget *page = gtk_widget_get_first_child (GTK_WIDGET (stack));
       page; page = gtk_widget_get_next_sibling (page)) {
    if (page != active)
      gtk_widget_set_visible (page, FALSE);
  }
}

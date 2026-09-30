#pragma once

#include <gtk/gtk.h>

/* Keep inactive pages out of CSS validation as well as out of painting. */
void spotifygtk_page_stack_show (GtkStack *stack, const gchar *name);
void spotifygtk_page_stack_hide_inactive (GtkStack *stack);

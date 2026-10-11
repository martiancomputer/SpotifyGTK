#pragma once

#include <gtk/gtk.h>

/* Optional prototype protocol: no fork-only exported symbols are linked. */
gboolean spotifygtk_frame_policy_supported (GtkWidget *widget);
void spotifygtk_frame_policy_bind (GtkWidget *window);

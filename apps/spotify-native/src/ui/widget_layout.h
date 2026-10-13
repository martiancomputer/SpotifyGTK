#pragma once

#include <gtk/gtk.h>
#include "layout_policy.h"

static inline void
spotifygtk_widget_measure_fit (GtkWidget *widget, GtkOrientation orientation,
                               gint for_size, gint *minimum, gint *natural,
                               gint *minimum_baseline, gint *natural_baseline)
{
  if (for_size >= 0) {
    gint cross_minimum = 0;
    gtk_widget_measure (widget,
      orientation == GTK_ORIENTATION_VERTICAL ? GTK_ORIENTATION_HORIZONTAL
                                              : GTK_ORIENTATION_VERTICAL,
      -1, &cross_minimum, NULL, NULL, NULL);
    for_size = spotifygtk_layout_measure_extent (for_size, cross_minimum);
  }
  gtk_widget_measure (widget, orientation, for_size, minimum, natural,
                      minimum_baseline, natural_baseline);
}

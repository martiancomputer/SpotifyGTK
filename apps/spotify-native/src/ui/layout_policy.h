#pragma once

#include <glib.h>

/* GTK's constrained measurement requires the cross-axis minimum, even when
 * a recycled cell still has its previous, narrower allocation. */
static inline gint
spotifygtk_layout_measure_extent (gint available, gint minimum)
{
  return available < 0 ? -1 : MAX (available, minimum);
}

static inline guint
spotifygtk_layout_columns (gint width, gint item_minimum)
{
  return MAX (1u, (guint) (MAX (width, 0) / MAX (item_minimum, 1)));
}

/* A row's slot includes the card's margins and the list-row's CSS box. The
 * explicit width request on the button must exclude that extra space. */
static inline gint
spotifygtk_layout_card_request (gint slot, gint row_minimum, gint base_request)
{
  return MAX (base_request, slot - MAX (0, row_minimum - base_request));
}

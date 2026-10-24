#pragma once

#include <glib.h>

/* Viewport scans and cover delivery must run before idle redraw sources.
 * Sustained rendering can otherwise starve even an already-decoded image. */
#define SPOTIFYGTK_ARTWORK_PRIORITY G_PRIORITY_DEFAULT

typedef struct {
  guint first;
  guint last; /* inclusive */
} SpotifyGtkArtworkWindow;

/* Anchor read-ahead to the viewport. A distant wheel destination may increase
 * the leading margin, but cannot skip approaching rows or grow it unboundedly.
 * The caller supplies a nonempty model and valid visible indices. */
static inline SpotifyGtkArtworkWindow
spotifygtk_layout_artwork_window (guint count, guint visible_first,
                                  guint visible_last, guint destination_first,
                                  guint ahead, guint max_ahead, guint trailing,
                                  gboolean upwards)
{
  guint distance = destination_first > visible_first
    ? destination_first - visible_first : visible_first - destination_first;
  ahead = MIN (ahead, max_ahead);
  ahead += MIN (distance, max_ahead - ahead);
  guint before = upwards ? ahead : trailing;
  guint after = upwards ? trailing : ahead;
  return (SpotifyGtkArtworkWindow) {
    visible_first - MIN (visible_first, before),
    visible_last + MIN (count - 1 - visible_last, after)
  };
}

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

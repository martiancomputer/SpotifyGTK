/*
 * smooth_scroll.h — animated mouse-wheel scrolling for any GtkScrolledWindow.
 *
 * Dragging a scrollbar moves its adjustment continuously, one position per
 * frame. A mouse wheel delivers discrete notches that GtkScrolledWindow applies
 * as instantaneous jumps. The difference is visible: the same view feels smooth
 * under the scrollbar and steppy under the wheel.
 *
 * This treats a notch as a destination rather than a displacement and eases
 * toward it per frame, so both inputs produce the same motion.
 *
 * Touchpads are left to GTK. Their deltas are already continuous and GTK's own
 * handling has kinetic follow-through, which this would fight.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Attach to `scroller`. `orientation` is the axis that actually scrolls:
 * VERTICAL for an ordinary list or grid, HORIZONTAL for a shelf.
 *
 * A horizontal target also captures the vertical wheel, because a wheel only
 * ever reports dy and a shelf would otherwise ignore it entirely.
 *
 * The event is consumed only while that axis can still move, so a scroller
 * nested in another one hands the wheel back at its ends instead of trapping
 * it. Safe to call once per scroller; state lives and dies with the widget.
 */
void spotifygtk_smooth_scroll_attach (GtkScrolledWindow *scroller,
                                      GtkOrientation     orientation);

G_END_DECLS

/*
 * context_menu.h — Shared right-click menu construction.
 *
 * Track rows and album cards want the same menu: a flat, left-aligned list of
 * actions in a popover anchored at the pointer. That was written once inside
 * track_list.c, which meant adding a menu anywhere else required copying the
 * popover lifetime handling — the part with the sharp edges, since the popover
 * has to outlive a recycled row and unparent itself when dismissed.
 *
 * Adding an entry is now one call to spotifygtk_context_menu_add(), which is
 * what makes the menu cheap to extend.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A menu under construction. Not a widget — pass it to _present(). */
typedef struct _SpotifyGtkContextMenu SpotifyGtkContextMenu;

SpotifyGtkContextMenu *spotifygtk_context_menu_new (void);

/*
 * Append one entry.
 *
 * `enabled` false shows the entry insensitive rather than hiding it: an action
 * a track cannot support should say so, not silently vanish. `tooltip` may be
 * NULL, and is worth setting on a disabled entry to explain why.
 *
 * `cb` is a GtkButton::clicked handler; it is only connected when enabled.
 */
void spotifygtk_context_menu_add (SpotifyGtkContextMenu *menu,
                                  const gchar           *label,
                                  gboolean               enabled,
                                  const gchar           *tooltip,
                                  GCallback              cb,
                                  gpointer               cb_data);

/* A non-interactive divider between groups of entries. */
void spotifygtk_context_menu_add_separator (SpotifyGtkContextMenu *menu);

/*
 * Pop the menu up at (x, y) within `anchor`, and hand it `ctx` — arbitrary
 * per-menu state retrievable by handlers via
 * spotifygtk_context_menu_get_context(), freed with `ctx_free` when the menu
 * goes away.
 *
 * Owning that state here is the point: a row can be recycled while its menu is
 * open, so anything the handlers need must be a copy that dies with the menu
 * rather than a pointer into the widget that spawned it.
 *
 * Consumes `menu`.
 */
void spotifygtk_context_menu_present (SpotifyGtkContextMenu *menu,
                                      GtkWidget             *anchor,
                                      gdouble                x,
                                      gdouble                y,
                                      gpointer               ctx,
                                      GDestroyNotify         ctx_free);

/* From inside a menu entry's callback: the state passed to _present(), and the
 * popover itself so a handler can dismiss it. */
gpointer    spotifygtk_context_menu_get_context (GtkWidget *entry_button);
GtkPopover *spotifygtk_context_menu_get_popover (GtkWidget *entry_button);

G_END_DECLS

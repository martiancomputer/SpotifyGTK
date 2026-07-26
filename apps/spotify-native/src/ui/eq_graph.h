/*
 * eq_graph.h — Draggable equaliser response curve.
 *
 * Replaces the column of vertical sliders. A slider strip shows what each band
 * is set to but not what the filter actually does; this draws the combined
 * magnitude response of the whole cascade, so band overlap and the summed
 * result are visible, and the handles are dragged directly on the curve.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_EQ_GRAPH (spotifygtk_eq_graph_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkEqGraph, spotifygtk_eq_graph,
                      SPOTIFYGTK, EQ_GRAPH, GtkDrawingArea)

SpotifyGtkEqGraph *spotifygtk_eq_graph_new (void);

/* Replace every band gain at once (loading settings, or Reset). Does not emit
 * band-changed — the caller already knows. */
void spotifygtk_eq_graph_set_gains (SpotifyGtkEqGraph *self, const gdouble *gains_db);

/* Signal: band-changed (guint band, gdouble gain_db) — emitted while dragging. */

G_END_DECLS

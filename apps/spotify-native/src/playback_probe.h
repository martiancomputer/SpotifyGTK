/*
 * playback_probe.h — Diagnostic for the "playback stops working after a few
 * tracks" report. See playback_probe.c for what it does and how to read it.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Runs the probe to completion. Returns 0 if every iteration reached
 * PLAYING, 1 if the wedge reproduced. */
int spotifygtk_run_playback_probe (const gchar *spec);

G_END_DECLS

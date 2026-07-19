/*
 * theme.h — Milk / White / Grey / Vanta Black theme system.
 *
 * Palette source: two of the four (Milk, Grey) were lifted exactly
 * from reference artwork the project owner provided -- a pair of
 * generative "technical dossier" pieces (monospace industrial
 * readouts, hairline 1px strokes, sharp corners, subtle grain,
 * *no* separate accent hue -- emphasis via inverted ink/background
 * fill rather than a third color). White and Vanta Black were
 * designed to complete that same family rather than invented
 * separately: White is the "no warmth, no coolness" neutral light
 * variant; Vanta is the dark inverse (true black, AMOLED-correct --
 * real black, not a dark grey, since the whole point of an
 * OLED-targeted theme is pixels that are actually off).
 *
 * Implementation: each theme is a set of libadwaita's own named
 * color overrides (@define-color window_bg_color, accent_bg_color,
 * etc.) -- the same mechanism libadwaita's own light.css/dark.css
 * use internally, not a workaround. Existing widgets that already
 * use standard libadwaita style classes (e.g. now_playing.c's play
 * button, styled "suggested-action") re-theme automatically with no
 * changes needed there -- "suggested-action" already pulls from
 * accent_bg_color/accent_fg_color under the hood.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  SPOTIFYGTK_THEME_MILK,
  SPOTIFYGTK_THEME_WHITE,
  SPOTIFYGTK_THEME_GREY,
  SPOTIFYGTK_THEME_VANTA,
} SpotifyGtkThemeName;

/* Applies the given theme's CSS to the default GdkDisplay, replacing
 * whatever was applied by a previous call (safe to call repeatedly,
 * e.g. from a settings UI, without stacking stale providers). */
void spotifygtk_theme_apply (SpotifyGtkThemeName theme);

/* Parses a GSettings-stored theme name string ("milk"/"white"/"grey"/
 * "vanta") into the enum, defaulting to SPOTIFYGTK_THEME_VANTA for
 * anything unrecognized (matches the schema's own default). */
SpotifyGtkThemeName spotifygtk_theme_from_string (const gchar *name);
const gchar         *spotifygtk_theme_to_string   (SpotifyGtkThemeName theme);

G_END_DECLS

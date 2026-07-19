/*
 * theme.c — theme CSS generation and application.
 * See theme.h for the palette sourcing/design rationale.
 */

#include "theme.h"
#include <string.h>

typedef struct {
  const gchar *bg;               /* window_bg_color */
  const gchar *view_bg;          /* view_bg_color -- content areas */
  const gchar *fg;               /* window_fg_color / view_fg_color */
  const gchar *headerbar_bg;
  const gchar *headerbar_fg;
  const gchar *card_bg;
  const gchar *card_fg;
  const gchar *popover_bg;
  const gchar *popover_fg;
  const gchar *sidebar_bg;
  const gchar *sidebar_fg;
  const gchar *border;           /* hairline stroke color, not a standard libadwaita name */
  const gchar *accent_bg;        /* inverted-ink-fill primary action, per the reference art's
                                  * "solid badge, background-colored text" emphasis device */
  const gchar *accent_fg;
  const gchar *now_playing_accent; /* the one restrained pop of real color per theme,
                                    * used only for the currently-playing indicator --
                                    * everything else stays two-tone (bg/ink), matching
                                    * the reference art's own restraint */
} ThemePalette;

/* Milk and Grey below are lifted exactly (bg/ink pair) from reference
 * artwork provided directly -- not designed from scratch. White and
 * Vanta were designed to complete the same family. See theme.h. */

static const ThemePalette PALETTE_MILK = {
  .bg = "#EFE9D7", .view_bg = "#F5F0E4", .fg = "#26231D",
  .headerbar_bg = "#EFE9D7", .headerbar_fg = "#26231D",
  .card_bg = "#F7F3E8", .card_fg = "#26231D",
  .popover_bg = "#F7F3E8", .popover_fg = "#26231D",
  .sidebar_bg = "#E6DFCC", .sidebar_fg = "#26231D",
  .border = "#D8CFB8",
  .accent_bg = "#26231D", .accent_fg = "#EFE9D7",
  .now_playing_accent = "#A6653A",
};

static const ThemePalette PALETTE_WHITE = {
  .bg = "#FFFFFF", .view_bg = "#FAFAFA", .fg = "#191919",
  .headerbar_bg = "#FFFFFF", .headerbar_fg = "#191919",
  .card_bg = "#F5F5F5", .card_fg = "#191919",
  .popover_bg = "#FFFFFF", .popover_fg = "#191919",
  .sidebar_bg = "#F0F0F0", .sidebar_fg = "#191919",
  .border = "#E2E2E2",
  .accent_bg = "#191919", .accent_fg = "#FFFFFF",
  .now_playing_accent = "#1DB954",
};

static const ThemePalette PALETTE_GREY = {
  .bg = "#999FA3", .view_bg = "#A3A9AC", .fg = "#22282B",
  .headerbar_bg = "#999FA3", .headerbar_fg = "#22282B",
  .card_bg = "#A8AEB1", .card_fg = "#22282B",
  .popover_bg = "#A8AEB1", .popover_fg = "#22282B",
  .sidebar_bg = "#8D9396", .sidebar_fg = "#22282B",
  .border = "#7B8286",
  .accent_bg = "#22282B", .accent_fg = "#999FA3",
  .now_playing_accent = "#3D6B6E",
};

static const ThemePalette PALETTE_VANTA = {
  /* True black, not dark grey -- an AMOLED theme that isn't actually
   * #000000 defeats its own purpose (no pixels actually turn off). */
  .bg = "#000000", .view_bg = "#000000", .fg = "#F2F2F2",
  .headerbar_bg = "#000000", .headerbar_fg = "#F2F2F2",
  .card_bg = "#121212", .card_fg = "#F2F2F2",
  .popover_bg = "#161616", .popover_fg = "#F2F2F2",
  .sidebar_bg = "#0A0A0A", .sidebar_fg = "#F2F2F2",
  .border = "#1E1E1E",
  .accent_bg = "#F2F2F2", .accent_fg = "#000000",
  .now_playing_accent = "#3DDC72",
};

static const ThemePalette *
palette_for (SpotifyGtkThemeName theme)
{
  switch (theme) {
    case SPOTIFYGTK_THEME_MILK:  return &PALETTE_MILK;
    case SPOTIFYGTK_THEME_WHITE: return &PALETTE_WHITE;
    case SPOTIFYGTK_THEME_GREY:  return &PALETTE_GREY;
    case SPOTIFYGTK_THEME_VANTA: return &PALETTE_VANTA;
    default:                     return &PALETTE_VANTA;
  }
}

static gchar *
build_css (const ThemePalette *p)
{
  /* @define-color overrides use libadwaita's own named colors -- the
   * same mechanism its light.css/dark.css use internally. Widgets
   * already styled with standard classes (e.g. now_playing.c's play
   * button, "suggested-action") re-theme automatically since that
   * class already pulls from accent_bg_color/accent_fg_color.
   *
   * Beyond colors: sharp-but-not-zero corners (2px -- true 0 causes
   * focus-ring rendering glitches in some libadwaita widgets) and
   * hairline 1px borders on frame-like elements, honoring the
   * reference art's technical-dossier aesthetic. A .spotifygtk-mono
   * utility class is provided for numeric/technical readouts (track
   * duration, bitrate, etc.) to opt into monospace, and
   * .spotifygtk-now-playing-dot carries the one restrained accent
   * color per theme for a "currently playing" indicator. */
  return g_strdup_printf (
    "@define-color window_bg_color %s;\n"
    "@define-color window_fg_color %s;\n"
    "@define-color view_bg_color %s;\n"
    "@define-color view_fg_color %s;\n"
    "@define-color headerbar_bg_color %s;\n"
    "@define-color headerbar_fg_color %s;\n"
    "@define-color headerbar_border_color %s;\n"
    "@define-color headerbar_backdrop_color %s;\n"
    "@define-color card_bg_color %s;\n"
    "@define-color card_fg_color %s;\n"
    "@define-color dialog_bg_color %s;\n"
    "@define-color dialog_fg_color %s;\n"
    "@define-color popover_bg_color %s;\n"
    "@define-color popover_fg_color %s;\n"
    "@define-color sidebar_bg_color %s;\n"
    "@define-color sidebar_fg_color %s;\n"
    "@define-color accent_bg_color %s;\n"
    "@define-color accent_fg_color %s;\n"
    "@define-color accent_color %s;\n"
    "@define-color borders %s;\n"
    "@define-color scrollbar_outline_color %s;\n"
    "\n"
    "/* Sharp-cornered, hairline-bordered technical-dossier aesthetic,\n"
    " * per the reference artwork -- not libadwaita's default ~12px\n"
    " * rounded soft-shadow look. */\n"
    ".card, list.boxed-list, list.boxed-list > row, entry,\n"
    ".navigation-sidebar row, .now-playing-bar {\n"
    "  border-radius: 2px;\n"
    "}\n"
    ".card, list.boxed-list, entry {\n"
    "  border: 1px solid %s;\n"
    "}\n"
    "\n"
    ".spotifygtk-mono {\n"
    "  font-family: monospace;\n"
    "  font-feature-settings: \"tnum\" 1;\n"
    "}\n"
    "\n"
    ".spotifygtk-now-playing-dot {\n"
    "  color: %s;\n"
    "}\n",
    p->bg, p->fg, p->view_bg, p->fg,
    p->headerbar_bg, p->headerbar_fg, p->border, p->headerbar_bg,
    p->card_bg, p->card_fg,
    p->card_bg, p->card_fg,
    p->popover_bg, p->popover_fg,
    p->sidebar_bg, p->sidebar_fg,
    p->accent_bg, p->accent_fg, p->accent_bg,
    p->border, p->border,
    p->border,
    p->now_playing_accent
  );
}

static GtkCssProvider *current_provider = NULL;

void
spotifygtk_theme_apply (SpotifyGtkThemeName theme)
{
  GdkDisplay *display = gdk_display_get_default ();
  if (!display) {
    g_warning ("theme.c: no default GdkDisplay -- cannot apply theme");
    return;
  }

  if (current_provider) {
    gtk_style_context_remove_provider_for_display (display,
      GTK_STYLE_PROVIDER (current_provider));
    g_clear_object (&current_provider);
  }

  const ThemePalette *p = palette_for (theme);
  g_autofree gchar *css = build_css (p);

  current_provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (current_provider, css);
  gtk_style_context_add_provider_for_display (display,
    GTK_STYLE_PROVIDER (current_provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_message ("theme.c: applied theme '%s'", spotifygtk_theme_to_string (theme));
}

SpotifyGtkThemeName
spotifygtk_theme_from_string (const gchar *name)
{
  if (!name) return SPOTIFYGTK_THEME_VANTA;
  if (g_strcmp0 (name, "milk")  == 0) return SPOTIFYGTK_THEME_MILK;
  if (g_strcmp0 (name, "white") == 0) return SPOTIFYGTK_THEME_WHITE;
  if (g_strcmp0 (name, "grey")  == 0) return SPOTIFYGTK_THEME_GREY;
  if (g_strcmp0 (name, "vanta") == 0) return SPOTIFYGTK_THEME_VANTA;
  return SPOTIFYGTK_THEME_VANTA;
}

const gchar *
spotifygtk_theme_to_string (SpotifyGtkThemeName theme)
{
  switch (theme) {
    case SPOTIFYGTK_THEME_MILK:  return "milk";
    case SPOTIFYGTK_THEME_WHITE: return "white";
    case SPOTIFYGTK_THEME_GREY:  return "grey";
    case SPOTIFYGTK_THEME_VANTA: return "vanta";
    default:                     return "vanta";
  }
}

#pragma once
#include <gtk/gtk.h>
#include "settings.h"

gboolean spotifygtk_shortcut_parse (const gchar *text, guint *key, GdkModifierType *mods);
gboolean spotifygtk_shortcut_matches (const gchar *text, guint key, GdkModifierType mods);
gboolean spotifygtk_shortcut_is_typing (GtkWidget *focus);
gboolean spotifygtk_shortcut_allowed_while_typing (SpotifyGtkShortcut action,
                                                  guint key, GdkModifierType mods);
gint spotifygtk_shortcut_find_conflict (SpotifyGtkSettings *settings,
                                       SpotifyGtkShortcut action, const gchar *accelerator);
/* Owned NULL-terminated legends: modifiers and the actual key separately.
 * In particular Ctrl++ must render Ctrl and +, not lose the plus to splitting. */
GStrv spotifygtk_shortcut_keycap_labels (const gchar *text);
gboolean spotifygtk_shortcut_search_matches (const gchar *query,
                                             const gchar *label,
                                             const gchar *description,
                                             const gchar *accelerator);
void spotifygtk_shortcuts_restore_defaults (SpotifyGtkSettings *settings);
/* Compact settings actions; the searchable editor is created on Edit. */
GtkWidget *spotifygtk_shortcuts_manager_new (void);

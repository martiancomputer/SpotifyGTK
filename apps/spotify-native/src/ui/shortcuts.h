#pragma once
#include <gtk/gtk.h>
#include "settings.h"

gboolean spotifygtk_shortcut_parse (const gchar *text, guint *key, GdkModifierType *mods);
gboolean spotifygtk_shortcut_matches (const gchar *text, guint key, GdkModifierType mods);
gboolean spotifygtk_shortcut_is_typing (GtkWidget *focus);
/* Only these three actions are exposed for now. */
GtkWidget *spotifygtk_shortcuts_editor_new (void);

#pragma once

#include <adwaita.h>

/* Linux-only, live whole-content zoom using native GTK drawing.
 * Overflow at large zoom remains reachable instead of clipping controls. */
GtkWidget *spotifygtk_presentation_wrap (GtkWidget *child);
/* Release the content from one of our non-default wrappers before reparenting
 * it back into the window. The caller must hold a reference to the content. */
void spotifygtk_presentation_detach (GtkWidget *wrapper);
/* Dialogs live outside AdwApplicationWindow's content subtree. */
void spotifygtk_presentation_wrap_dialog (AdwDialog *dialog);

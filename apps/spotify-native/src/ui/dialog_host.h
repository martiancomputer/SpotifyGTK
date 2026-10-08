#pragma once

#include <adwaita.h>

/* Use embedded, non-draggable dialogs consistently throughout the app. */
void spotifygtk_dialog_prepare (AdwDialog *dialog);
void spotifygtk_dialog_host_bind (AdwApplicationWindow *window);

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Embedded, borderless artwork preview. No decoded-image cache or history. */
void spotifygtk_artwork_viewer_present (GtkWidget *parent,
                                        const gchar *cover_id,
                                        const gchar *title);

G_END_DECLS

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Return 0=off, 1=ordinary shuffle, 2=Smart Shuffle. */
guint spotifygtk_connect_player_shuffle_mode  (const guint8 *options,
                                               gsize len,
                                               guint fallback);
guint spotifygtk_connect_command_shuffle_mode (const guint8 *options,
                                               gsize len,
                                               guint fallback);

G_END_DECLS

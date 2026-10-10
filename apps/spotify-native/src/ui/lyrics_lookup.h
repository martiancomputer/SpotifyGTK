#pragma once

#include <json-glib/json-glib.h>

/* Borrowed result, or NULL. Search results are not trustworthy matches until
 * their recording metadata and duration have been checked. No I/O or GTK. */
JsonObject *spotifygtk_lyrics_select_match (JsonArray *results,
                                           const gchar *title,
                                           const gchar *artists,
                                           const gchar *album,
                                           gint64 duration_ms);

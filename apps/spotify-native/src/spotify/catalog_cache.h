#pragma once
#include <gio/gio.h>

/* Small, disposable Spotify catalogue snapshots, not credentials/user data.
 * No process-wide payload cache: only callers hold the bytes they read. */
GBytes *spotifygtk_catalog_cache_get (const gchar *key, guint max_age_seconds);
void spotifygtk_catalog_cache_put (const gchar *key, GBytes *bytes, guint epoch);
void spotifygtk_catalog_cache_remove (const gchar *key);
void spotifygtk_catalog_cache_clear (void);
guint spotifygtk_catalog_cache_epoch (void);
void spotifygtk_catalog_cache_set_enabled (gboolean enabled);
gchar *spotifygtk_catalog_context_key (const gchar *uri, guint limit);
void spotifygtk_catalog_context_invalidate (const gchar *uri);

/* Compact card identity. Empty/error titles never replace a good identity. */
gboolean spotifygtk_catalog_card_get (const gchar *uri, gchar **name, gchar **cover);
gboolean spotifygtk_catalog_card_is_fresh (const gchar *uri, guint max_age_seconds);
void spotifygtk_catalog_card_put (const gchar *uri, const gchar *name,
                                 const gchar *cover, guint epoch);

/*
 * image_cache.h — Two-layer LRU + disk image cache for album art.
 *
 * L1: in-memory GdkTexture cache (LRU-evicted, ~200 entries)
 * L2: on-disk cache keyed by SHA1(url), survives restarts
 * Decode tiers (fastest first, all runtime-probed):
 *   1. VA-API hardware JPEG decode  (nightly, optional, zero shader-core cost)
 *   2. libjpeg-turbo SIMD decode    (CPU, any track)
 *   3. stb_image (vendored)        (universal fallback, always works)
 *
 * All network/decode work happens off the GTK main thread; results
 * land back via g_idle_add() so callers never block.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*ImageReadyCallback) (GdkTexture *texture, gpointer user_data);

#define SPOTIFYGTK_TYPE_IMAGE_CACHE (spotifygtk_image_cache_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyImageCache, spotifygtk_image_cache,
                      SPOTIFYGTK, IMAGE_CACHE, GObject)

SpotifyImageCache *spotifygtk_image_cache_new (void);

/* Returns immediately. If already cached (L1), callback fires
 * synchronously before this function returns. Otherwise it fires
 * later on the main loop once the image is fetched/decoded. Callers
 * should show a placeholder immediately and not assume synchronous
 * delivery. */
void spotifygtk_image_cache_get (SpotifyImageCache *self, const gchar *url,
                                 ImageReadyCallback callback, gpointer user_data);

/* Diagnostic — which decode backend got selected at startup probe. */
const gchar *spotifygtk_image_cache_active_backend (SpotifyImageCache *self);

G_END_DECLS

/*
 * cover_loader.h — Album artwork fetching, decoding and caching.
 *
 * Covers arrive as a hex image id on SpotifyNativeTrack (extracted from
 * Track.album.cover_group by track_meta.c) and are served from Spotify's
 * public image CDN at https://i.scdn.co/image/<id>.
 *
 * Why not the image cache in spotify-connect: that one is built around the
 * Web API's JSON image URLs and carries a VA-API/libjpeg-turbo decode ladder
 * this does not need. GTK4 already decodes JPEG into a GdkTexture, and a
 * texture is what the widgets want, so this stays small rather than sharing
 * a component whose extra machinery would be unused here.
 *
 * Caching is by image id, in memory, for the process lifetime. Album art is
 * small, heavily repeated within a listing (every track of an album shares
 * one), and immutable for a given id — so a plain hash table is enough, and
 * a listing of 100 tracks typically resolves to a handful of fetches.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Called with the decoded texture, or NULL if the cover could not be
 * fetched or decoded. The texture is owned by the cache; ref it to keep it.
 *
 * Always invoked on the thread that made the request.
 */
typedef void (*SpotifyCoverCallback) (GdkTexture *texture, gpointer user_data);

/*
 * Request the cover for `cover_id` (the hex id from SpotifyNativeTrack),
 * decoded to at most `target_px` on its longest edge.
 *
 * The size matters for memory, not just crispness: a cover decoded at its
 * native 640 square is 1.6 MB, and a full list of them doubled the process
 * RSS. A 40px row thumbnail has no use for more than ~96px, so it asks for
 * that and the decoded texture is ~1/40th the size. The panel, which shows
 * the cover large, asks for a large target. Textures are cached per
 * (id, target), so the small and large decodes of one album coexist without
 * one clobbering the other.
 *
 * A cached cover invokes the callback before returning. `cover_id` NULL or
 * empty invokes it with NULL, so callers do not need to special-case a
 * track whose album has no artwork.
 *
 * Cancelling stops the callback from running, which is what list rows need:
 * rows are recycled as results arrive, and a late callback would otherwise
 * paint one track's artwork onto another's row.
 */
void spotifygtk_cover_load (const gchar          *cover_id,
                            gint                  target_px,
                            GCancellable         *cancellable,
                            SpotifyCoverCallback  callback,
                            gpointer              user_data);

/* Build the CDN URL for an image id. Exposed for testing. */
gchar *spotifygtk_cover_build_url (const gchar *cover_id);

/*
 * While TRUE, a cache miss is dropped instead of fetched. Cache hits are still
 * served, so rows whose art is already known still paint.
 *
 * For use during a fast scroll: binding every row a fling passes over would
 * otherwise queue hundreds of fetches for rows nobody will look at, competing
 * with the handful that end up on screen. Callers are expected to clear this
 * when the scroll settles and re-request what is actually visible.
 */
void spotifygtk_cover_set_deferred (gboolean deferred);
gboolean spotifygtk_cover_get_deferred (void);

/* Warm the cache without delivering anywhere. Honours deferral, so a prefetch
 * issued while scrolling costs nothing. */
void spotifygtk_cover_prefetch (const gchar *cover_id, gint target_px);

G_END_DECLS

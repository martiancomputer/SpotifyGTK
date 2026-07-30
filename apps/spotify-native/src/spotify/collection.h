/*
 * collection.h — Adding to and removing from the user's collection ("Liked
 * Songs"), over the native stack.
 *
 * WHY THIS IS NOT THE WEB API
 *
 * `PUT /v1/me/tracks` would do this in one line, but this client deliberately
 * does not talk to api.spotify.com at all (see the README's catalog-data-path
 * note). The native route is Mercury, the same request/response channel
 * Spotify Connect already uses over the live AP connection.
 *
 * THE MESSAGE IS KNOWN, THE ENDPOINT IS NOT
 *
 * The payload is `WriteRequest` from collection2v2.proto, extracted from a real
 * Spotify desktop build:
 *
 *     message CollectionItem { string uri = 1; int32 added_at = 2;
 *                              bool is_removed = 3; optional string context_uri = 4; }
 *     message WriteRequest    { string username = 1; string set = 2;
 *                               repeated CollectionItem items = 3;
 *                               string client_update_id = 4; }
 *
 * `is_removed` is precisely the like/unlike bit, and `set` is "collection" for
 * Liked Songs. A track URI or an album URI both go in `uri`.
 *
 * What is *not* known is the Mercury URI this is sent to. librespot implements
 * collection reads only and has no write path anywhere in its tree, so there is
 * nothing to copy: its endpoints follow `hm://<service>/v<n>/<resource>`
 * (`hm://playlist/v2/playlist/`, `hm://connect-state/v1/...`), but the
 * collection one is not among them.
 *
 * That is why the endpoint is a *parameter* here rather than a constant baked
 * into the call: candidates can be tried from the harness without a rebuild,
 * and nothing in the UI is wired to a URI that has never returned 200. The
 * encoding, by contrast, is pinned byte-for-byte in tests/test_collection.c, so
 * a failing write can be attributed to the endpoint rather than the payload.
 */

#pragma once

#include <glib.h>
#include "mercury.h"

G_BEGIN_DECLS

/* The "set" a collection item belongs to. Liked Songs is "collection". */
#define SPOTIFYGTK_COLLECTION_SET_LIKED "collection"

/*
 * Encode a WriteRequest. Exposed separately from the send so it can be tested
 * without a network or an AP session — proto3 omits default-valued fields, so
 * `added_at` of 0 and `is_removed` of FALSE are absent from the output rather
 * than written as zeroes, which is what a real client produces.
 *
 * Returns a new GByteArray; free with g_byte_array_unref().
 */
GByteArray *spotifygtk_collection_build_write (const gchar        *username,
                                               const gchar        *set,
                                               const gchar *const *uris,
                                               guint               n_uris,
                                               gint32              added_at,
                                               gboolean            is_removed);

/* ok is TRUE only for a 2xx Mercury status; `status` carries the raw code so a
 * caller can tell "wrong endpoint" from "rejected". */
typedef void (*SpotifyCollectionCallback) (gboolean ok, guint16 status,
                                           gpointer user_data);

/*
 * Send a collection write. `endpoint` is the full `hm://...` URI — see the
 * header comment on why it is not hardcoded.
 */
void spotifygtk_collection_write (SpotifyMercury            *mercury,
                                  const gchar               *endpoint,
                                  const gchar               *username,
                                  const gchar               *set,
                                  const gchar *const        *uris,
                                  guint                      n_uris,
                                  gboolean                   is_removed,
                                  SpotifyCollectionCallback  callback,
                                  gpointer                   user_data);

G_END_DECLS

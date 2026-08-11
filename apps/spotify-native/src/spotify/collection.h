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
 * What is *not* known is the Mercury URI this is sent to -- in either
 * direction. An earlier note here said librespot implements collection reads;
 * it does not. collection2v2.proto sits in its tree but is absent from
 * protocol/build.rs and referenced by no Rust file, so it is carried as a
 * captured schema and never used. There is no reference implementation to copy
 * for reads or writes.
 *
 * What can be said is the shape of its other endpoints,
 * `hm://<service>/v<n>/<resource>` (`hm://playlist/v2/playlist/`,
 * `hm://connect-state/v1/...`).
 *
 * That is why the endpoint is a *parameter* here rather than a constant baked
 * into the call: candidates can be tried without a rebuild, and nothing in the
 * UI is wired to a URI that has never returned 200. The encoding, by contrast,
 * is pinned byte-for-byte in tests/test_collection.c, so a failing write can be
 * attributed to the endpoint rather than the payload.
 *
 * NOTHING HERE HAS EVER BEEN SENT
 *
 * An earlier version of this comment described a "candidate list in the
 * harness probe". No such probe exists, and one cannot be written yet: the
 * Mercury layer is send-only. spotifygtk_mercury_request() encodes a packet,
 * files the callback under its sequence number in `pending`, and hands it to
 * the AP -- but no handler is registered for AP_CMD_MERCURY_REQ (0xb2) or
 * AP_CMD_MERCURY_EVENT (0xb5), so nothing ever decodes a reply, drains
 * `pending`, or invokes a callback. spotifygtk_mercury_new() is never called
 * from anywhere either, so no instance exists at runtime.
 *
 * Implementing the receive half is therefore a prerequisite for testing any
 * endpoint candidate, and for connect.c, which has the same problem.
 */

#pragma once

#include <glib.h>
#include "mercury.h"

G_BEGIN_DECLS

/* The "set" a collection item belongs to. Liked Songs is "collection". */
#define SPOTIFYGTK_COLLECTION_SET_LIKED "collection"

/*
 * Followed artists are their own set. Saved albums are not: they go in
 * "collection", the same set as liked songs, distinguished only by the URI.
 *
 * Both proven against the throwaway account -- a write returning 200 and the
 * item present on the read back, then removed and absent again. The string
 * table lists "album" alongside "artist" and it looks like the obvious name,
 * but reading or writing that set answers 403; the guess was wrong and only
 * the live check caught it. Readable sets are collection, artist and show.
 * See research/library-writes.md.
 */
#define SPOTIFYGTK_COLLECTION_SET_ARTISTS "artist"
#define SPOTIFYGTK_COLLECTION_SET_ALBUMS  SPOTIFYGTK_COLLECTION_SET_LIKED

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
/* ── Reading a page of the collection ─────────────────────────────────────
 *
 * PageRequest -> PageResponse from the same schema. This is what carries
 * `added_at`, which /context-resolve does not: resolving the collection URI
 * returns the tracks in added order but no timestamps, so Liked Songs can be
 * sorted by date and cannot show one.
 */

/* One entry of a PageResponse. `uri` is owned by the response array. */
typedef struct {
  gchar   *uri;
  gint32   added_at;    /* seconds since epoch; 0 when absent */
  gboolean is_removed;
} SpotifyCollectionItem;

void spotifygtk_collection_items_free (SpotifyCollectionItem *items, guint n_items);

/*
 * Encode a PageRequest. Separate from the send for the same reason the write
 * encoder is: it can be pinned byte-for-byte in a test with no AP session, so
 * a failing request is attributable to the endpoint rather than the payload.
 *
 * `pagination_token` may be NULL for the first page. Returns a new GByteArray;
 * free with g_byte_array_unref().
 */
GByteArray *spotifygtk_collection_build_page_request (const gchar *username,
                                                      const gchar *set,
                                                      const gchar *pagination_token,
                                                      gint32       limit);

/*
 * Parse a PageResponse. Returns FALSE only if the buffer is unusable; a
 * response with no items is a legitimate empty page and returns TRUE with
 * *n_items == 0.
 *
 * *next_page_token is newly allocated, or NULL when the last page is reached.
 */
gboolean spotifygtk_collection_parse_page_response (const guint8           *data,
                                                    gsize                   len,
                                                    SpotifyCollectionItem **items,
                                                    guint                  *n_items,
                                                    gchar                 **next_page_token);

/* ok is TRUE only for a 2xx Mercury status. Items and token are owned by the
 * callee and valid for the duration of the callback. */
typedef void (*SpotifyCollectionPageCallback) (gboolean               ok,
                                               guint16                status,
                                               SpotifyCollectionItem *items,
                                               guint                  n_items,
                                               const gchar           *next_page_token,
                                               gpointer               user_data);

void spotifygtk_collection_read_page (SpotifyMercury                *mercury,
                                      const gchar                   *endpoint,
                                      const gchar                   *username,
                                      const gchar                   *set,
                                      const gchar                   *pagination_token,
                                      gint32                         limit,
                                      SpotifyCollectionPageCallback  callback,
                                      gpointer                       user_data);

void spotifygtk_collection_write (SpotifyMercury            *mercury,
                                  const gchar               *endpoint,
                                  const gchar               *username,
                                  const gchar               *set,
                                  const gchar *const        *uris,
                                  guint                      n_uris,
                                  gboolean                   is_removed,
                                  SpotifyCollectionCallback  callback,
                                  gpointer                   user_data);

/* ── collection v2 ─────────────────────────────────────────────────────────
 *
 * The service the official client uses, recovered from its binary and proven
 * live. Everything above this line talks to hm://collection/collection/<user>,
 * which is legacy: it replaces the whole set on every write, caps out around
 * 576 items, and answers in a schema no published proto matches.
 *
 * This one is additive, paginated, and speaks collection2v2 exactly:
 *
 *     POST hm://collection/v2/{write,paging,initialized}
 *     Header.content_type = SPOTIFYGTK_COLLECTION_V2_CONTENT_TYPE
 *
 * The content type is not optional -- without it the server answers in the
 * legacy shape.
 */

#define SPOTIFYGTK_COLLECTION_V2_CONTENT_TYPE \
  "application/vnd.collection-v2.spotify.proto"

/*
 * Add or remove `uris`. Additive: a write names only what changes and leaves
 * the rest of the collection alone, which is the whole difference from the
 * legacy endpoint.
 */
void spotifygtk_collection_v2_write (SpotifyMercury            *mercury,
                                     const gchar               *username,
                                     const gchar               *set,
                                     const gchar *const        *uris,
                                     guint                      n_uris,
                                     gboolean                   is_removed,
                                     SpotifyCollectionCallback  callback,
                                     gpointer                   user_data);

/* One page of the collection. `pagination_token` is NULL for the first page;
 * the callback receives the next one, or NULL at the end. */
void spotifygtk_collection_v2_read_page (SpotifyMercury                *mercury,
                                         const gchar                   *username,
                                         const gchar                   *set,
                                         const gchar                   *pagination_token,
                                         gint32                         limit,
                                         SpotifyCollectionPageCallback  callback,
                                         gpointer                       user_data);

/* ── Playlists: what is known so far ───────────────────────────────────────
 *
 * Probed read-only against a live account; no write has been attempted.
 *
 *   GET  hm://playlist/v2/user/<user>/rootlist           -> 200
 *   GET  hm://playlist/v2/user/<user>/publishedrootlist  -> 200
 *   GET  hm://playlist/user/<user>/rootlist              -> 200  (older form)
 *   GET  hm://playlist/v2/playlist/<id>                  -> 200
 *   GET  hm://playlist/v2/rootlist                       -> 404
 *
 * Responses are playlist4 SelectedListContent: a revision, a length, list
 * attributes, then contents. Liked Songs is itself a playlist under this
 * service (37i9dQZF1F5p3rmiWPIYgZ) and reads correctly, but a playlist4 ADD op
 * against it returns 200 and changes nothing -- a pseudo-playlist that accepts
 * writes and discards them. A real playlist is expected to behave differently
 * and has not been tried.
 *
 * Mutation looks like playlist4 ops posted to <playlist uri>/changes:
 *
 *   ListChanges { base_revision = 1, deltas = 2 }
 *   Delta       { base_version = 1, ops = 2 }
 *   Op          { kind = 1 (ADD = 2), add = 2 }
 *   Add         { from_index = 1, items = 2, add_last = 4 }
 *   Item        { uri = 1, attributes = 2 }
 *   ItemAttributes { added_by = 1, timestamp = 2, ... }
 *
 * ItemAttributes.timestamp means a playlist add can carry its own date, which
 * the collection write cannot express for anything but the current time.
 *
 * The official binary names the operations at its internal layer --
 * playlist_create, playlist_add_tracks -- and carries
 * PlaylistModificationInfo { uri = 1, new_revision = 2, parent_revision = 3 },
 * whose revision pair is the optimistic-concurrency scheme: a change names the
 * revision it was based on, and is rejected if the list has moved on.
 *
 * CREATE AND ADD, BOTH PROVEN
 *
 *   POST hm://playlist/v2/playlist
 *   body: SelectedListContent { attributes = 3 { name = 1 } }
 *   -> 200, CreateListReply { uri = 1, revision = 2 }
 *
 * PUT is refused with 405; POST is the verb. The reply carries the new URI in
 * full ("spotify:playlist:<22 chars>") and a 24-byte revision.
 *
 * A created list is NOT in the user's library. The rootlist stays empty until
 * the new URI is added to it -- making the list and filing it are separate
 * operations, which is worth knowing before wondering why a successful create
 * leaves nothing on screen.
 *
 *   POST hm://playlist/v2/playlist/<id>/changes
 *   body: ListChanges { base_revision, deltas[ Delta { ops[ Op { kind=ADD,
 *         add { items[ Item { uri } ], add_last = true } } ] } ] }
 *   -> 200, and the list's length moves 0 -> 3
 *
 * Add.from_index and Add.add_last are ALTERNATIVES. Sending both is a 400 with
 * nothing to say which field is at fault; sending only add_last works. That one
 * detail is the difference between the op being accepted and rejected, and it
 * is not stated in the schema.
 */

G_END_DECLS

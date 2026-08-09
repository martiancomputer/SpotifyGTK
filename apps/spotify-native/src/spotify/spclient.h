/*
 * spclient.h — HTTPS client for Spotify's "spclient" service:
 * track metadata and, most importantly for basic playback, CDN URL
 * resolution for a given audio file_id.
 *
 * Host resolution: librespot resolves this dynamically via
 * apresolve.spotify.com (a JSON API, itself with a hardcoded
 * fallback: spclient.wg.spotify.com:443, per core/src/apresolve.rs).
 * This module uses that same fallback host directly rather than
 * implementing the full apresolve flow -- a deliberate, honestly-
 * scoped simplification for a first working version, not an oversight.
 * Tracked as a follow-up in research/playback/.
 *
 * Every request needs a login5-obtained bearer token (see login5.h)
 * as Authorization, plus a client-token (see clienttoken.h) --
 * confirmed against spclient.rs's request_with_options().
 */

#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "track_meta.h"

G_BEGIN_DECLS

/* Fallback host librespot itself uses when apresolve is unavailable
 * or not implemented -- see file header. */
#define SPCLIENT_FALLBACK_HOST "spclient.wg.spotify.com:443"

/* ── CDN URL resolution ──────────────────────────────────────────────────── */

typedef struct {
  gchar **urls;      /* NULL-terminated array of CDN URLs to try, in order */
  guint   n_urls;
} SpclientCdnUrls;

typedef void (*SpclientStorageCallback) (SpclientCdnUrls *urls /* NULL on failure, owned -- see free fn */,
                                         GError *error,
                                         gpointer user_data);

/* ── Track metadata ─────────────────────────────────────────────────────── */

/* One AudioFile entry from Track.file[] in metadata.proto.
 * format values per AudioFile::Format enum (proto2 field 2):
 *   OGG_VORBIS_96=0, OGG_VORBIS_160=1, OGG_VORBIS_320=2,
 *   MP3_256=3, MP3_320=4, AAC_24=8, AAC_48=9, FLAC_FLAC=16 */
typedef struct {
  guint8  file_id[20];   /* raw 20-byte FileId -- same format audio_key.h + get_audio_storage use */
  guint8  track_gid[16]; /* 16-byte raw GID of the track this file belongs to */
  guint64 format;        /* AudioFile::Format varint value */
} SpclientAudioFile;

typedef void (*SpclientTrackCallback) (const SpclientAudioFile *files /* NULL on failure */,
                                       guint                    n_files,
                                       GError                  *error,
                                       gpointer                 user_data);

/* ── Batched display metadata ────────────────────────────────────────────── */

/*
 * Display metadata for one track, correlated back to the URI that asked
 * for it. `entity_uri` comes from EntityExtensionData.entity_uri in the
 * response rather than from request ordering, so a partial or reordered
 * response still lines up with the right row.
 */
typedef struct {
  gchar            *entity_uri;
  SpotifyTrackMeta  meta;
} SpclientTrackInfo;

typedef void (*SpclientBatchCallback) (const SpclientTrackInfo *tracks /* NULL on failure */,
                                       guint                    n_tracks,
                                       GError                  *error,
                                       gpointer                 user_data);

/* Prototype lives with the rest of the API, below the GObject declaration. */

/* ── Context resolution ──────────────────────────────────────────────────── */

/*
 * /context-resolve/v1/{uri} — the single endpoint behind search, liked
 * songs, playlist contents, album and artist track listings.
 *
 * Unlike everything else in this file the response is JSON, not protobuf:
 * librespot fetches it with request_with_options and then parses the body
 * as the JSON encoding of the Context message (spclient.rs get_context).
 *
 * Special URI forms, per spclient.rs's own doc comment:
 *   - search:      spotify:search:<query>   (spaces replaced with '+')
 *   - liked songs: spotify:user:<user_id>:collection
 *   - anything addressable by SpotifyId: playlist, album, artist, track
 *
 * IMPORTANT — what this does and does not give you. The Context message
 * (context.proto / context_page.proto / context_track.proto) carries
 * pages[].tracks[] where each ContextTrack has uri, uid, gid and a
 * metadata string map. That map is NOT a source of display metadata:
 * librespot reads exactly one key from it, "is_queued". Track names,
 * artists, album and duration come from a second call to the
 * extended-metadata endpoint, parsed by track_meta.h. Treat this as a
 * URI list, not a track listing.
 */
typedef void (*SpclientContextCallback) (JsonNode *context /* NULL on failure, owned by caller */,
                                         GError   *error,
                                         gpointer  user_data);

/* ── GObject type ────────────────────────────────────────────────────────── */

#define SPOTIFYGTK_TYPE_SPCLIENT (spotifygtk_spclient_get_type ())
G_DECLARE_FINAL_TYPE (SpotifySpclient, spotifygtk_spclient, SPOTIFYGTK, SPCLIENT, GObject)

SpotifySpclient *spotifygtk_spclient_new (void);
void spotifygtk_spclient_set_cancellable (SpotifySpclient *, GCancellable *);

/* ── API ─────────────────────────────────────────────────────────────────── */

/*
 * Fetch track metadata for `track_uri` (e.g. "spotify:track:<base62>").
 * Calls /extended-metadata/v0/extended-metadata (POST, proto3) with
 * ExtensionKind TRACK_V4=10. On success, invokes callback with an
 * array of SpclientAudioFile structs (all AudioFile entries from the
 * Track proto). The array is owned by the implementation and is only
 * valid for the duration of the callback.
 *
 * bearer_token: login5 access_token ("Bearer ..." prefix added here).
 * client_token: from clienttoken.h (nullable -- tolerated as absent
 * per spclient.rs, but should be provided for best compatibility).
 */
void spotifygtk_spclient_get_track_metadata (SpotifySpclient      *self,
                                              const gchar          *track_uri,
                                              const gchar          *bearer_token,
                                              const gchar          *client_token,
                                              SpclientTrackCallback callback,
                                              gpointer              user_data);

/*
 * Resolves CDN URLs for a given audio file_id (raw 20-byte
 * FileId/GID, as found in track metadata -- the same identifier
 * audio_key.h's spotifygtk_audio_key_request() takes for file_id).
 * bearer_token: from login5.h. client_token: from clienttoken.h
 * (may be NULL -- unlike login5, get_audio_storage tolerates its
 * absence per spclient.rs's documented behavior).
 */
void spotifygtk_spclient_get_audio_storage (SpotifySpclient *self,
                                             const guint8 *file_id, gsize file_id_len,
                                             const gchar *bearer_token,
                                             const gchar *client_token /* nullable */,
                                             SpclientStorageCallback callback,
                                             gpointer user_data);

/*
 * Fetch display metadata for many tracks in a single request.
 *
 * BatchedEntityRequest.entity_request is `repeated` (extended_metadata.proto),
 * so a page of search results costs one round trip rather than one per row.
 * This is the mandatory second half of context resolution: /context-resolve
 * returns nothing but URIs, so nothing can be rendered without this call.
 *
 * BATCH SIZE: there is an upper bound, found by probing a real account
 * rather than documented upstream. Confirmed working at 50, 200, 500,
 * 1000 and 2000 URIs in one request; a 4,773-track liked-songs collection
 * in a single request comes back unparseable. The exact ceiling sits
 * somewhere between 2000 and 4773 and is not worth pinning down, because
 * callers should be chunking to a page size long before that. Treat a few
 * hundred as the comfortable working size and page the rest.
 *
 * The array passed to the callback is owned by the implementation and is
 * only valid for the duration of the callback.
 */
void spotifygtk_spclient_get_tracks_metadata (SpotifySpclient      *self,
                                               const gchar *const   *track_uris,
                                               guint                 n_uris,
                                               const gchar          *bearer_token,
                                               const gchar          *client_token /* nullable */,
                                               SpclientBatchCallback callback,
                                               gpointer              user_data);

/*
 * Resolve `context_uri` (see SpclientContextCallback above for the
 * accepted forms). On success the callback receives the parsed JSON
 * root node, which it takes ownership of.
 *
 * bearer_token: login5 access_token. client_token: from clienttoken.h.
 */
/*
 * The artist's portrait image id, from the extended-metadata service.
 *
 * A resolved track carries its album's cover and nothing else, so an artist
 * page has no image of the artist unless it asks for one. The same batch
 * endpoint answers for any entity -- it takes an entity URI and an extension
 * kind -- so this is that endpoint with ARTIST_V4 instead of TRACK_V4.
 *
 * `cover_id` is a hex Image.file_id ready for the cover loader, or NULL when
 * the artist has no portrait. Owned by the callee.
 */
typedef void (*SpclientArtistCallback) (const gchar *cover_id /* NULL if none */,
                                        GError      *error    /* NULL on success */,
                                        gpointer     user_data);


void spotifygtk_spclient_get_artist_portrait (SpotifySpclient        *self,
                                              const gchar            *artist_uri,
                                              const gchar            *bearer_token,
                                              const gchar            *client_token,
                                              SpclientArtistCallback  callback,
                                              gpointer                user_data);

void spotifygtk_spclient_get_context (SpotifySpclient        *self,
                                       const gchar            *context_uri,
                                       const gchar            *bearer_token,
                                       const gchar            *client_token /* nullable */,
                                       SpclientContextCallback callback,
                                       gpointer                user_data);

/*
 * Build a `spotify:search:<query>` context URI. Spaces become '+' per
 * spclient.rs; everything else is percent-escaped so a query containing
 * ':' or '/' cannot alter the URI's structure or the request path.
 *
 * Returns a newly-allocated string, or NULL if `query` is NULL/empty.
 */
gchar *spotifygtk_spclient_build_search_uri (const gchar *query);

/*
 * Build a `spotify:user:<user_id>:collection` context URI — the liked
 * songs listing. `user_id` is the canonical username from APWelcome
 * (spotifygtk_ap_session_get_username).
 */
gchar *spotifygtk_spclient_build_collection_uri (const gchar *user_id);

void spclient_cdn_urls_free (SpclientCdnUrls *urls);

G_END_DECLS

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

/* ── GObject type ────────────────────────────────────────────────────────── */

#define SPOTIFYGTK_TYPE_SPCLIENT (spotifygtk_spclient_get_type ())
G_DECLARE_FINAL_TYPE (SpotifySpclient, spotifygtk_spclient, SPOTIFYGTK, SPCLIENT, GObject)

SpotifySpclient *spotifygtk_spclient_new (void);

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

void spclient_cdn_urls_free (SpclientCdnUrls *urls);

G_END_DECLS

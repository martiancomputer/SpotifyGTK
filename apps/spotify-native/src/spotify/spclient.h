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

typedef struct {
  gchar **urls;      /* NULL-terminated array of CDN URLs to try, in order */
  guint   n_urls;
} SpclientCdnUrls;

typedef void (*SpclientStorageCallback) (SpclientCdnUrls *urls /* NULL on failure, owned -- see free fn */,
                                         GError *error,
                                         gpointer user_data);

#define SPOTIFYGTK_TYPE_SPCLIENT (spotifygtk_spclient_get_type ())
G_DECLARE_FINAL_TYPE (SpotifySpclient, spotifygtk_spclient, SPOTIFYGTK, SPCLIENT, GObject)

SpotifySpclient *spotifygtk_spclient_new (void);

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

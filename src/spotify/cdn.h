/*
 * cdn.h — Encrypted audio chunk fetching from Spotify's CDN.
 *
 * Unlike the AP connection, this is plain HTTPS (libsoup3) — no
 * custom protocol here. The CDN URL comes from a Mercury track
 * metadata lookup; chunks are fetched with Range requests and
 * decrypted client-side with AES-128-CTR using the key from
 * audio_key.c, then handed to audio/decoder.c as Ogg/Vorbis bytes.
 */

#pragma once

#include <glib-object.h>
#include "audio_key.h"

G_BEGIN_DECLS

#define CDN_CHUNK_SIZE (16 * 1024)   /* bytes per Range request */

typedef void (*CdnChunkCallback) (GBytes *decrypted_chunk, GError *error, gpointer user_data);

#define SPOTIFYGTK_TYPE_CDN_FETCHER (spotifygtk_cdn_fetcher_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyCdnFetcher, spotifygtk_cdn_fetcher, SPOTIFYGTK, CDN_FETCHER, GObject)

SpotifyCdnFetcher *spotifygtk_cdn_fetcher_new (void);

/* Fetches byte range [offset, offset+length) from cdn_url, decrypts
 * it in place with AES-128-CTR using `key`, and the CTR counter
 * derived from `offset` (CTR mode lets us seek without decrypting
 * from the start — essential for scrubbing playback position). */
void spotifygtk_cdn_fetch_chunk (SpotifyCdnFetcher *self,
                                 const gchar *cdn_url,
                                 const guint8 key[AUDIO_KEY_LEN],
                                 goffset offset, gsize length,
                                 CdnChunkCallback callback, gpointer user_data);

G_END_DECLS

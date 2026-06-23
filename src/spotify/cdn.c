/*
 * cdn.c — CDN chunk fetching + AES-128-CTR decryption.
 *
 * This part of the pipeline is "boring" by design — plain HTTPS Range
 * requests via libsoup3, decrypted with standard AES-128-CTR via
 * OpenSSL's EVP API. No Spotify-specific framing to reverse-engineer
 * here, which is why it's implemented fully rather than stubbed.
 *
 * One open question flagged rather than guessed at: some audio file
 * formats historically used by Spotify prepend a small unencrypted
 * header before the encrypted Ogg stream begins (so "byte offset 0 in
 * the decrypted stream" isn't necessarily "byte offset 0 in the CDN
 * file"). That header size needs to be confirmed against a real
 * fetched file before wiring this into decoder.c — see the
 * STREAM_HEADER_OFFSET constant below.
 */

#include "config.h"
#include "cdn.h"

#include <libsoup/soup.h>
#if HAVE_OPENSSL
#include <openssl/evp.h>
#endif
#include <string.h>

/* TODO: confirm against a real captured file before relying on this. */
#define STREAM_HEADER_OFFSET 0

struct _SpotifyCdnFetcher {
  GObject      parent_instance;
  SoupSession *session;
};

G_DEFINE_FINAL_TYPE (SpotifyCdnFetcher, spotifygtk_cdn_fetcher, G_TYPE_OBJECT)

typedef struct {
  SpotifyCdnFetcher *self;
  CdnChunkCallback   callback;
  gpointer           user_data;
  guint8             key[AUDIO_KEY_LEN];
  goffset            offset;
} FetchClosure;

#if HAVE_OPENSSL
static GBytes *
aes_ctr_decrypt (const guint8 *key, goffset stream_offset, const guint8 *ciphertext, gsize len)
{
  /* CTR mode IV = 16-byte counter, big-endian, starting block derived
   * from stream_offset (block size 16 bytes for AES). Spotify's IV
   * seed value itself is part of what needs confirming against a real
   * stream — using an all-zero IV here as the structurally-correct
   * placeholder so chunk alignment/seeking logic can be validated
   * independently of the exact seed bytes. */
  guint8 iv[16] = {0};
  guint64 block_offset = (guint64) (stream_offset / 16);
  for (int i = 0; i < 8; i++)
    iv[15 - i] = (guint8) (block_offset >> (8 * i));

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new ();
  EVP_DecryptInit_ex (ctx, EVP_aes_128_ctr (), NULL, key, iv);

  guint8 *out = g_malloc (len);
  int outlen = 0;
  EVP_DecryptUpdate (ctx, out, &outlen, ciphertext, (int) len);
  EVP_CIPHER_CTX_free (ctx);

  return g_bytes_new_take (out, (gsize) outlen);
}
#endif

static void
on_range_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  FetchClosure *cl = user_data;
  g_autoptr(GError) err = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  if (!bytes) {
    cl->callback (NULL, err, cl->user_data);
    g_free (cl);
    return;
  }

#if HAVE_OPENSSL
  gsize len = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);
  GBytes *decrypted = aes_ctr_decrypt (cl->key, cl->offset + STREAM_HEADER_OFFSET, data, len);
  cl->callback (decrypted, NULL, cl->user_data);
  g_bytes_unref (decrypted);
#else
  g_warning ("cdn.c: built without OpenSSL — cannot decrypt audio chunks");
  GError *no_ssl = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                        "OpenSSL not available at build time");
  cl->callback (NULL, no_ssl, cl->user_data);
  g_error_free (no_ssl);
#endif

  g_bytes_unref (bytes);
  g_free (cl);
}

void
spotifygtk_cdn_fetch_chunk (SpotifyCdnFetcher *self, const gchar *cdn_url,
                            const guint8 key[AUDIO_KEY_LEN], goffset offset, gsize length,
                            CdnChunkCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_CDN_FETCHER (self));

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, cdn_url);

  g_autofree gchar *range = g_strdup_printf ("bytes=%" G_GOFFSET_FORMAT "-%" G_GOFFSET_FORMAT,
                                             offset, offset + (goffset) length - 1);
  soup_message_headers_replace (soup_message_get_request_headers (msg), "Range", range);

  FetchClosure *cl = g_new0 (FetchClosure, 1);
  cl->self      = self;
  cl->callback  = callback;
  cl->user_data = user_data;
  cl->offset    = offset;
  memcpy (cl->key, key, AUDIO_KEY_LEN);

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, NULL,
                                    on_range_response, cl);
  g_object_unref (msg);
}

static void
spotifygtk_cdn_fetcher_dispose (GObject *object)
{
  SpotifyCdnFetcher *self = SPOTIFYGTK_CDN_FETCHER (object);
  g_clear_object (&self->session);
  G_OBJECT_CLASS (spotifygtk_cdn_fetcher_parent_class)->dispose (object);
}

static void
spotifygtk_cdn_fetcher_class_init (SpotifyCdnFetcherClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_cdn_fetcher_dispose;
}

static void
spotifygtk_cdn_fetcher_init (SpotifyCdnFetcher *self)
{
  self->session = soup_session_new_with_options ("user-agent", "SpotifyGTK/" APP_VERSION, NULL);
}

SpotifyCdnFetcher *
spotifygtk_cdn_fetcher_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_CDN_FETCHER, NULL);
}

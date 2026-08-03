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

/* Spotify's custom Ogg packet with normalisation data is 167 bytes. */
#define STREAM_HEADER_OFFSET 167

struct _SpotifyCdnFetcher {
  GObject      parent_instance;
  SoupSession *session;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (SpotifyCdnFetcher, spotifygtk_cdn_fetcher, G_TYPE_OBJECT)

/*
 * Our own deadline per range request.
 *
 * libsoup's SoupSession:timeout does not rescue this case -- observed live, a
 * request outstanding for over three minutes with the property set to 20. The
 * connection was in CLOSE-WAIT: the CDN had closed its side during a pause and
 * libsoup reused the corpse from its pool, writing the request into a socket
 * whose peer was already gone. Writing succeeds there, so nothing fails; the
 * response simply never arrives.
 *
 * So do not depend on the library's semantics. Arm a timer per request against
 * a cancellable of our own, and if it fires, cancel -- which surfaces as an
 * ordinary error the read-ahead path already recovers from by re-resolving and
 * retrying.
 */
/*
 * Sized against the largest range actually requested, not against a guess.
 *
 * This was 12s while the read-ahead chunk was raised to 256KB in the same
 * session -- and 256KB on a slow link legitimately takes longer than that
 * (13s at 20KB/s). So the deadline began killing transfers that were slow but
 * perfectly healthy, turning a survivable slowdown into a dead stream. It has
 * to clear the worst case for a real chunk by a wide margin, because the cost
 * of firing too early is far higher than the cost of waiting.
 */
#define CDN_REQUEST_DEADLINE_S 45

typedef struct {
  SpotifyCdnFetcher *self;
  SoupMessage       *message;
  CdnChunkCallback   callback;
  gpointer           user_data;
  guint8             key[AUDIO_KEY_LEN];
  goffset            offset;
  gsize              length;

  GCancellable      *cancel;        /* this request only */
  gboolean           reported;      /* the caller has already been told */
  gulong             parent_id;     /* handler on the shared cancellable */
  GSource           *deadline;
  gboolean           timed_out;
} FetchClosure;

static void
fetch_closure_disarm (FetchClosure *cl)
{
  if (cl->deadline) {
    g_source_destroy (cl->deadline);
    g_source_unref (cl->deadline);
    cl->deadline = NULL;
  }
  if (cl->parent_id && cl->self && cl->self->cancellable) {
    g_cancellable_disconnect (cl->self->cancellable, cl->parent_id);
    cl->parent_id = 0;
  }
  g_clear_object (&cl->cancel);
}

static gboolean
on_request_deadline (gpointer user_data)
{
  FetchClosure *cl = user_data;

  /* Drop our ref here: returning G_SOURCE_REMOVE destroys the source, and GLib
   * holds its own ref for the dispatch. */
  GSource *fired = cl->deadline;
  cl->deadline = NULL;
  if (fired)
    g_source_unref (fired);

  cl->timed_out = TRUE;
  g_warning ("cdn: no response for logical offset %" G_GOFFSET_FORMAT " after %ds "
             "-- reporting the failure directly", cl->offset, CDN_REQUEST_DEADLINE_S);

  /*
   * Report the failure from here rather than waiting for the cancelled request
   * to complete. This is the third design for this, and the first backed by
   * evidence instead of by what the API ought to do.
   *
   * With entry logging in the response callback, a live session showed it
   * running normally for every successful range and *never* running after a
   * deadline cancelled one -- 45 seconds and counting, with the loop still
   * iterating (the deadline itself had just fired on it). So cancelling a
   * stuck send_and_read_async does not complete it, and every recovery built
   * on "cancel, then handle the error in the callback" could not work.
   *
   * So the deadline is the recovery trigger, not a way to make libsoup report
   * one. `reported` makes that safe: if the operation does eventually complete,
   * the callback sees the caller has already been told and only cleans up.
   *
   * Ordering matters. The session is aborted *before* the callback runs,
   * because the callback re-resolves and issues a fresh request and aborting
   * after would kill that one too. And the callback is invoked through locals
   * rather than through `cl`, because the abort may complete this operation
   * synchronously, which frees the closure.
   */
  cl->reported = TRUE;

  CdnChunkCallback callback   = cl->callback;
  gpointer         caller_data = cl->user_data;
  goffset          offset      = cl->offset;

  g_cancellable_cancel (cl->cancel);

  /* Purge the pool: the connection that went quiet is unusable, and libsoup
   * would otherwise hand the same one to the retry. */
  if (cl->self && cl->self->session)
    soup_session_abort (cl->self->session);
  /* `cl` may be freed from here on. */

  if (callback) {
    g_autoptr(GError) err = g_error_new (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                         "no response within %ds for logical offset %"
                                         G_GOFFSET_FORMAT " (connection abandoned)",
                                         CDN_REQUEST_DEADLINE_S, offset);
    callback (NULL, err, caller_data);
  }

  return G_SOURCE_REMOVE;
}

#if HAVE_OPENSSL
static GBytes *
aes_ctr_decrypt (const guint8 *key, goffset stream_offset, const guint8 *ciphertext, gsize len)
{
  guint8 iv[16] = {
    0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
    0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93
  };
  
  guint64 block_offset = (guint64) (stream_offset / 16);
  guint8 intra_block_offset = stream_offset % 16;

  if (block_offset > 0) {
    guint64 carry = block_offset;
    for (int i = 15; i >= 0 && carry > 0; i--) {
      guint64 sum = iv[i] + (carry & 0xFF);
      iv[i] = sum & 0xFF;
      carry = (carry >> 8) + (sum >> 8);
    }
  }

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new ();
  EVP_DecryptInit_ex (ctx, EVP_aes_128_ctr (), NULL, key, iv);

  if (intra_block_offset > 0) {
    guint8 dummy_in[16] = {0};
    guint8 dummy_out[16] = {0};
    int dummy_len = 0;
    EVP_DecryptUpdate (ctx, dummy_out, &dummy_len, dummy_in, intra_block_offset);
  }

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

  /* Entry logging on purpose: the open question is whether this callback runs
   * at all after a deadline cancels the request. Two deadlines were observed
   * with no sign of the read-ahead failure path afterwards, which means either
   * this never ran or it ran and went somewhere unexpected. One line settles
   * that; without it the next occurrence is as unreadable as the last. */
  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  gboolean timed_out = cl->timed_out;
  gboolean reported  = cl->reported;
  g_message ("cdn: response callback for logical offset %" G_GOFFSET_FORMAT
             " (bytes=%s, timed_out=%s, err=%s)",
             cl->offset, bytes ? "yes" : "no", timed_out ? "yes" : "no",
             err ? err->message : "none");
  fetch_closure_disarm (cl);

  /* The deadline already told the caller and already purged the pool. Anything
   * arriving now is a late answer to a request that has been given up on:
   * clean up and say nothing, or the caller would be handed a second result
   * for a range it has already moved past. */
  if (reported) {
    g_clear_object (&cl->message);
    g_free (cl);
    if (bytes)
      g_bytes_unref (bytes);
    return;
  }

  if (!bytes) {
    /* Report a deadline as a deadline. Cancellation from a Stop looks identical
     * at this level, and the two want opposite responses -- one is retried, the
     * other means the user has moved on. */
    g_autoptr(GError) synthetic = NULL;
    if (timed_out) {
      synthetic = g_error_new (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                               "no response within %ds (connection abandoned)",
                               CDN_REQUEST_DEADLINE_S);
    }
    cl->callback (NULL, timed_out ? synthetic : err, cl->user_data);
    g_clear_object (&cl->message);
    g_free (cl);
    return;
  }

  guint status = soup_message_get_status (cl->message);
  if (status != SOUP_STATUS_PARTIAL_CONTENT) {
    GError *status_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                        "CDN range request returned HTTP %u (expected 206)", status);
    g_warning ("cdn: logical offset %" G_GOFFSET_FORMAT ", length %" G_GSIZE_FORMAT
               " returned HTTP %u instead of 206 Partial Content",
               cl->offset, cl->length, status);
    cl->callback (NULL, status_error, cl->user_data);
    g_error_free (status_error);
    g_bytes_unref (bytes);
    g_clear_object (&cl->message);
    g_free (cl);
    return;
  }

#if HAVE_OPENSSL
  gsize len = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);
  g_message ("cdn: received HTTP 206 for logical offset %" G_GOFFSET_FORMAT
             ", requested %" G_GSIZE_FORMAT ", received %" G_GSIZE_FORMAT " bytes",
             cl->offset, cl->length, len);
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
  g_clear_object (&cl->message);
  g_free (cl);
}

void
spotifygtk_cdn_fetch_chunk (SpotifyCdnFetcher *self, const gchar *cdn_url,
                            const guint8 key[AUDIO_KEY_LEN], goffset offset, gsize length,
                            CdnChunkCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_CDN_FETCHER (self));

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, cdn_url);

  goffset actual_offset = offset + STREAM_HEADER_OFFSET;
  g_autofree gchar *range = g_strdup_printf ("bytes=%" G_GOFFSET_FORMAT "-%" G_GOFFSET_FORMAT,
                                             actual_offset, actual_offset + (goffset) length - 1);
  soup_message_headers_replace (soup_message_get_request_headers (msg), "Range", range);

  FetchClosure *cl = g_new0 (FetchClosure, 1);
  cl->self      = self;
  cl->message   = g_object_ref (msg);
  cl->callback  = callback;
  cl->user_data = user_data;
  cl->offset    = offset;
  cl->length    = length;
  memcpy (cl->key, key, AUDIO_KEY_LEN);

  g_message ("cdn: requesting logical range %" G_GOFFSET_FORMAT "+%" G_GSIZE_FORMAT
             " (physical range starts at %" G_GOFFSET_FORMAT ")",
             offset, length, actual_offset);

  /* Chained to the engine's cancellable so a Stop still aborts this, while the
   * deadline below can cancel just this request without touching the rest. */
  cl->cancel = g_cancellable_new ();
  if (self->cancellable)
    cl->parent_id = g_cancellable_connect (self->cancellable,
                                           G_CALLBACK (g_cancellable_cancel),
                                           g_object_ref (cl->cancel),
                                           g_object_unref);

  cl->deadline = g_timeout_source_new_seconds (CDN_REQUEST_DEADLINE_S);
  g_source_set_callback (cl->deadline, on_request_deadline, cl, NULL);
  g_source_attach (cl->deadline, g_main_context_get_thread_default ());

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    cl->cancel,
                                    on_range_response, cl);
  g_object_unref (msg);
}

static void
spotifygtk_cdn_fetcher_dispose (GObject *object)
{
  SpotifyCdnFetcher *self = SPOTIFYGTK_CDN_FETCHER (object);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
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
  /*
   * Both timeouts are load-bearing, and their absence is what killed playback
   * after a long pause.
   *
   * libsoup pools connections and, with no timeout set, waits forever. Observed
   * live: a 73-second pause, then the range request issued on resume never got
   * an answer at all -- no 206, no error, nothing for 96 seconds until the user
   * gave up. The connection had gone away while idle (server or an intermediary
   * dropping it, silently, as they do), and the request went out on a socket
   * with nobody at the other end.
   *
   * idle-timeout is the actual fix: a pooled connection older than this is
   * closed rather than reused, so a resume after a pause opens a fresh one.
   * timeout is the backstop for a request that stalls anyway, and turns an
   * indefinite hang into an error -- which the read-ahead path already knows
   * how to recover from by re-resolving and retrying.
   *
   * Chunks arrive every second or two during playback, so idle-timeout is never
   * reached while audio is actually flowing; it only bites across a pause,
   * which is precisely when the connection is not worth keeping.
   */
  self->session = soup_session_new_with_options ("user-agent", "SpotifyGTK/" APP_VERSION,
                                                 "timeout", 20,
                                                 "idle-timeout", 15,
                                                 NULL);
}

SpotifyCdnFetcher *
spotifygtk_cdn_fetcher_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_CDN_FETCHER, NULL);
}

void
spotifygtk_cdn_fetcher_set_cancellable (SpotifyCdnFetcher *self,
                                        GCancellable *cancellable)
{
  g_return_if_fail (SPOTIFYGTK_IS_CDN_FETCHER (self));
  g_set_object (&self->cancellable, cancellable);
}

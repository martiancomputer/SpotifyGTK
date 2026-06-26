/*
 * decoder.c — Ogg/Vorbis -> PCM via libvorbisfile streaming callbacks.
 *
 * Standard, well-documented library usage — no protocol reverse
 * engineering involved, so implemented in full. The ring buffer lets
 * cdn.c feed compressed bytes incrementally as HTTPS Range chunks
 * arrive, rather than needing the whole track buffered first.
 */

#include "config.h"
#include "decoder.h"

#include <vorbis/vorbisfile.h>
#include <string.h>

#define RING_BUFFER_CAPACITY (1 << 20)   /* 1 MiB compressed-data ring */

struct _SpotifyDecoder {
  GObject       parent_instance;

  OggVorbis_File vf;
  gboolean       vf_open;
  vorbis_info   *info;

  /* Ring buffer of not-yet-consumed compressed bytes */
  guint8        *ring;
  gsize          ring_head;   /* read position */
  gsize          ring_tail;   /* write position */
  gsize          ring_fill;   /* bytes currently buffered */

  gboolean       eof_fed;     /* caller signalled no more data coming */
};

G_DEFINE_FINAL_TYPE (SpotifyDecoder, spotifygtk_decoder, G_TYPE_OBJECT)

/* ── ov_callbacks backed by the ring buffer ──────────────────────────────── */

static size_t
ring_read_cb (void *ptr, size_t size, size_t nmemb, void *datasource)
{
  SpotifyDecoder *self = datasource;
  gsize want = size * nmemb;
  gsize avail = self->ring_fill;
  gsize take = MIN (want, avail);

  for (gsize i = 0; i < take; i++) {
    ((guint8 *) ptr)[i] = self->ring[self->ring_head];
    self->ring_head = (self->ring_head + 1) % RING_BUFFER_CAPACITY;
  }
  self->ring_fill -= take;

  return take / size;  /* libvorbisfile expects element count */
}

/* Seeking within a live network stream isn't meaningful at the ring-buffer
 * level (we'd need to re-request a CDN byte range instead) — report
 * non-seekable so libvorbisfile treats this as a forward-only stream. */
static int
ring_seek_cb (void *datasource, ogg_int64_t offset, int whence)
{
  (void) datasource; (void) offset; (void) whence;
  return -1;
}

static long
ring_tell_cb (void *datasource)
{
  (void) datasource;
  return -1;
}

static int
ring_close_cb (void *datasource)
{
  (void) datasource;
  return 0;
}

static const ov_callbacks RING_CALLBACKS = {
  .read_func  = ring_read_cb,
  .seek_func  = ring_seek_cb,
  .close_func = ring_close_cb,
  .tell_func  = ring_tell_cb,
};

/* ── Public API ───────────────────────────────────────────────────────────── */

void
spotifygtk_decoder_feed (SpotifyDecoder *self, GBytes *ogg_bytes)
{
  g_return_if_fail (SPOTIFYGTK_IS_DECODER (self));

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (ogg_bytes, &len);

  gsize space = RING_BUFFER_CAPACITY - self->ring_fill;
  if (len > space) {
    g_warning ("decoder: ring buffer overflow, dropping %" G_GSIZE_FORMAT " bytes",
              len - space);
    len = space;
  }

  for (gsize i = 0; i < len; i++) {
    self->ring[self->ring_tail] = data[i];
    self->ring_tail = (self->ring_tail + 1) % RING_BUFFER_CAPACITY;
  }
  self->ring_fill += len;

  if (!self->vf_open && self->ring_fill > 0) {
    if (ov_open_callbacks (self, &self->vf, NULL, 0, RING_CALLBACKS) == 0) {
      self->vf_open = TRUE;
      self->info = ov_info (&self->vf, -1);
    }
    /* Non-zero return just means "not enough data yet to find the
     * Vorbis headers" — caller will feed more and we retry next call. */
  }
}

PcmFrame *
spotifygtk_decoder_pull (SpotifyDecoder *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_DECODER (self), NULL);
  if (!self->vf_open) return NULL;

  float **pcm = NULL;
  long samples = ov_read_float (&self->vf, &pcm, 4096, NULL);

  if (samples <= 0) return NULL;  /* need more compressed data, or real EOF */

  gint channels = self->info ? self->info->channels : 2;

  PcmFrame *frame = g_new0 (PcmFrame, 1);
  frame->n_frames    = (gsize) samples;
  frame->channels    = channels;
  frame->sample_rate = self->info ? self->info->rate : 44100;
  frame->samples     = g_new (gint16, (gsize) samples * (gsize) channels);

  for (long i = 0; i < samples; i++) {
    for (gint c = 0; c < channels; c++) {
      float v = pcm[c][i];
      if (v > 1.0f) v = 1.0f;
      if (v < -1.0f) v = -1.0f;
      frame->samples[i * channels + c] = (gint16) (v * 32767.0f);
    }
  }

  return frame;
}

void
pcm_frame_free (PcmFrame *frame)
{
  if (!frame) return;
  g_free (frame->samples);
  g_free (frame);
}

void
spotifygtk_decoder_reset (SpotifyDecoder *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_DECODER (self));
  if (self->vf_open) {
    ov_clear (&self->vf);
    self->vf_open = FALSE;
  }
  self->ring_head = self->ring_tail = self->ring_fill = 0;
  self->eof_fed = FALSE;
}

static void
spotifygtk_decoder_finalize (GObject *object)
{
  SpotifyDecoder *self = SPOTIFYGTK_DECODER (object);
  if (self->vf_open) ov_clear (&self->vf);
  g_free (self->ring);
  G_OBJECT_CLASS (spotifygtk_decoder_parent_class)->finalize (object);
}

static void
spotifygtk_decoder_class_init (SpotifyDecoderClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = spotifygtk_decoder_finalize;
}

static void
spotifygtk_decoder_init (SpotifyDecoder *self)
{
  self->ring = g_malloc (RING_BUFFER_CAPACITY);
}

SpotifyDecoder *
spotifygtk_decoder_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_DECODER, NULL);
}

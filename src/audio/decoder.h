/*
 * decoder.h — Ogg/Vorbis -> PCM decoding.
 *
 * Wraps libvorbisfile's streaming decode API. Audio arrives
 * incrementally from cdn.c (16KB chunks over HTTPS), so this uses
 * custom ov_callbacks backed by an internal ring buffer rather than
 * requiring the whole file up front — necessary for low startup
 * latency and for seeking without re-downloading from byte 0.
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_DECODER (spotifygtk_decoder_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyDecoder, spotifygtk_decoder, SPOTIFYGTK, DECODER, GObject)

typedef struct {
  gint16 *samples;     /* interleaved, 16-bit signed PCM */
  gsize   n_frames;    /* per-channel sample count */
  gint    channels;
  gint    sample_rate;
} PcmFrame;

SpotifyDecoder *spotifygtk_decoder_new (void);

/* Feed compressed Ogg/Vorbis bytes as they arrive from the CDN.
 * Internally buffered; safe to call with small chunks. */
void spotifygtk_decoder_feed (SpotifyDecoder *self, GBytes *ogg_bytes);

/* Pull the next decoded PCM frame, or NULL if more compressed data
 * is needed first (caller should feed more, then retry). */
PcmFrame *spotifygtk_decoder_pull (SpotifyDecoder *self);

void pcm_frame_free (PcmFrame *frame);

void spotifygtk_decoder_reset (SpotifyDecoder *self);

G_END_DECLS

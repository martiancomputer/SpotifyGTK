/*
 * resampler.h — Sample-rate conversion for the output path.
 *
 * Spotify's Ogg streams decode at 44.1 kHz. Until now the output was opened at
 * whatever the stream decoded to and the Sample rate setting was inert, because
 * choosing a rate without a converter would just mislabel the stream and play
 * it at the wrong speed. This is that converter.
 *
 * WHAT IT IS
 *
 * A polyphase windowed-sinc interpolator: a Kaiser-windowed sinc kernel is
 * precomputed at a fixed set of sub-sample phases, and each output sample is a
 * dot product of the kernel against a window of input samples, with the phase
 * chosen by where the output falls between two input samples. This is the
 * standard high-quality approach — the alternative, linear interpolation, is
 * cheap but audibly harsh because its stopband rejection is poor.
 *
 * WHEN DOWNSAMPLING the kernel cutoff moves below the output Nyquist, so the
 * band that would otherwise alias back into the audible range is filtered out
 * first. Upsampling keeps the full band.
 *
 * A NOTE ON WHETHER YOU WANT THIS AT ALL
 *
 * If the target rate equals the stream rate, this is bypassed entirely and the
 * samples reach the device untouched — that passthrough is the highest-fidelity
 * path and remains the default. Converting 44.1 kHz to anything else cannot add
 * information; at best it is transparent. It is worth enabling when the device
 * (or the sound server) would otherwise resample for you and you would rather
 * control how, and not otherwise.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SpotifyResampler SpotifyResampler;

/* `channels` is the interleave width (2 for stereo) and is fixed for the
 * resampler's life; rates are set separately and may change. */
SpotifyResampler *spotifygtk_resampler_new  (gint channels);
void              spotifygtk_resampler_free (SpotifyResampler *self);

/*
 * Set (or change) the conversion. Changing the rates rebuilds the kernel and
 * clears the filter history, so call it on a stream boundary rather than
 * mid-stream if you can — mid-stream it costs one kernel-length of transient.
 * Equal rates put the resampler in passthrough.
 */
void spotifygtk_resampler_set_rates (SpotifyResampler *self,
                                     gint in_rate, gint out_rate);

/* TRUE when in_rate == out_rate, i.e. process() would only copy. Callers use
 * this to skip the conversion entirely rather than pay for a no-op. */
gboolean spotifygtk_resampler_is_passthrough (SpotifyResampler *self);

/*
 * Convert `in_frames` of interleaved 16-bit PCM. On return `*out` points at a
 * freshly g_malloc'd buffer the caller owns (g_free it), holding the returned
 * number of frames, interleaved at the same channel count.
 *
 * Streaming-safe: the tail of each block is retained as filter history, so
 * consecutive calls join without a seam. Returns 0 (and sets *out to NULL) if
 * this block produced no complete output frame, which is normal for small
 * blocks when downsampling.
 */
gsize spotifygtk_resampler_process (SpotifyResampler *self,
                                    const gint16 *in, gsize in_frames,
                                    gint16 **out);

G_END_DECLS

/*
 * resampler.c — Polyphase windowed-sinc sample-rate conversion.
 *
 * See resampler.h for what this is and when it is worth using. The mechanics:
 *
 *   - The kernel is a sinc lowpass multiplied by a Kaiser window. Sinc alone is
 *     infinite and truncating it rings; the Kaiser window trades a slightly
 *     wider transition band for a much deeper stopband, which is what keeps
 *     aliasing inaudible.
 *   - It is evaluated at RESAMPLER_PHASES sub-sample offsets and stored, so the
 *     hot path is a dot product rather than a sin() per tap. Output positions
 *     rarely land exactly on a stored phase, so the two neighbouring phases are
 *     linearly interpolated — cheap, and it removes the phase-quantisation
 *     noise a nearest-phase lookup would leave.
 *   - Each phase row is normalised to sum to 1. Without that, DC (and therefore
 *     overall level) wobbles slightly as the phase walks, which shows up as a
 *     low-level buzz on sustained tones.
 */

#include "resampler.h"

#include <math.h>
#include <string.h>

/* 32 taps at 256 phases is the usual sweet spot for audio: enough taps for a
 * steep transition, few enough that the dot product stays cheap. */
#define RESAMPLER_TAPS    32
#define RESAMPLER_HALF    (RESAMPLER_TAPS / 2)
#define RESAMPLER_PHASES  256

/* Kaiser beta. ~8.6 puts the stopband near -90 dB, below 16-bit noise floor,
 * so the conversion cannot be the weakest link in a 16-bit path. */
#define RESAMPLER_BETA    8.6

/* Leave a little headroom below Nyquist for the transition band rather than
 * cutting exactly at it, which would either alias or ring. */
#define RESAMPLER_CUTOFF  0.92

struct _SpotifyResampler {
  gint     channels;
  gint     in_rate;
  gint     out_rate;
  gboolean passthrough;

  /* (PHASES + 1) rows so the interpolation between phase p and p+1 can read
   * one past the last without a bounds test in the inner loop. */
  gdouble  kernel[RESAMPLER_PHASES + 1][RESAMPLER_TAPS];

  /* Tail of the previous block, so blocks join seamlessly. */
  gint16  *history;        /* RESAMPLER_TAPS frames, interleaved */
  gdouble  pos;            /* next output position, in input-frame units */
};

static gdouble
sinc (gdouble x)
{
  if (fabs (x) < 1e-9)
    return 1.0;
  return sin (G_PI * x) / (G_PI * x);
}

/* Modified Bessel function of the first kind, order 0 — the Kaiser window's
 * defining term. The series converges fast for the arguments used here. */
static gdouble
bessel_i0 (gdouble x)
{
  gdouble sum = 1.0, term = 1.0;
  for (gint k = 1; k < 32; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum  += term;
    if (term < sum * 1e-14)
      break;
  }
  return sum;
}

static void
build_kernel (SpotifyResampler *self)
{
  /* Downsampling must band-limit to the *output* Nyquist, not the input's, or
   * everything above it folds back as alias. Upsampling keeps the full band. */
  gdouble ratio  = (gdouble) self->out_rate / (gdouble) self->in_rate;
  gdouble cutoff = RESAMPLER_CUTOFF * MIN (1.0, ratio);
  gdouble i0_beta = bessel_i0 (RESAMPLER_BETA);

  for (gint p = 0; p <= RESAMPLER_PHASES; p++) {
    gdouble frac = (gdouble) p / (gdouble) RESAMPLER_PHASES;
    gdouble sum  = 0.0;

    for (gint t = 0; t < RESAMPLER_TAPS; t++) {
      /* Distance, in input samples, from the output position to this tap. */
      gdouble d = frac + (RESAMPLER_HALF - 1) - t;

      /* Kaiser window argument, normalised to +-1 across the kernel. */
      gdouble r = d / (gdouble) RESAMPLER_HALF;
      gdouble w = 0.0;
      if (fabs (r) <= 1.0)
        w = bessel_i0 (RESAMPLER_BETA * sqrt (MAX (0.0, 1.0 - r * r))) / i0_beta;

      gdouble h = cutoff * sinc (cutoff * d) * w;
      self->kernel[p][t] = h;
      sum += h;
    }

    /* Unity DC gain per phase; see the file header for why this matters. */
    if (fabs (sum) > 1e-12) {
      for (gint t = 0; t < RESAMPLER_TAPS; t++)
        self->kernel[p][t] /= sum;
    }
  }
}

SpotifyResampler *
spotifygtk_resampler_new (gint channels)
{
  g_return_val_if_fail (channels > 0, NULL);

  SpotifyResampler *self = g_new0 (SpotifyResampler, 1);
  self->channels    = channels;
  self->passthrough = TRUE;
  self->history     = g_new0 (gint16, (gsize) RESAMPLER_TAPS * channels);
  self->pos         = RESAMPLER_HALF;
  return self;
}

void
spotifygtk_resampler_free (SpotifyResampler *self)
{
  if (!self)
    return;
  g_free (self->history);
  g_free (self);
}

void
spotifygtk_resampler_set_rates (SpotifyResampler *self, gint in_rate, gint out_rate)
{
  g_return_if_fail (self != NULL);

  if (in_rate <= 0 || out_rate <= 0)
    return;
  if (self->in_rate == in_rate && self->out_rate == out_rate)
    return;

  self->in_rate     = in_rate;
  self->out_rate    = out_rate;
  self->passthrough = (in_rate == out_rate);

  memset (self->history, 0, sizeof (gint16) * (gsize) RESAMPLER_TAPS * self->channels);
  self->pos = RESAMPLER_HALF;

  if (!self->passthrough)
    build_kernel (self);
}

gboolean
spotifygtk_resampler_is_passthrough (SpotifyResampler *self)
{
  return !self || self->passthrough;
}

gsize
spotifygtk_resampler_process (SpotifyResampler *self,
                              const gint16 *in, gsize in_frames,
                              gint16 **out)
{
  g_return_val_if_fail (self != NULL && out != NULL, 0);
  *out = NULL;

  if (!in || in_frames == 0)
    return 0;

  gint ch = self->channels;

  if (self->passthrough) {
    gsize bytes = sizeof (gint16) * in_frames * (gsize) ch;
    *out = g_malloc (bytes);
    memcpy (*out, in, bytes);
    return in_frames;
  }

  /* Work buffer: retained history followed by this block, so a tap window that
   * straddles the block boundary still sees real samples. */
  gsize hist_frames = RESAMPLER_TAPS;
  gsize tmp_frames  = hist_frames + in_frames;
  g_autofree gint16 *tmp = g_new (gint16, tmp_frames * (gsize) ch);
  memcpy (tmp, self->history, sizeof (gint16) * hist_frames * (gsize) ch);
  memcpy (tmp + hist_frames * (gsize) ch, in, sizeof (gint16) * in_frames * (gsize) ch);

  gdouble step = (gdouble) self->in_rate / (gdouble) self->out_rate;

  /* The last position whose tap window still lies inside tmp. */
  gdouble limit = (gdouble) tmp_frames - RESAMPLER_HALF;

  gsize capacity = (gsize) ((limit - self->pos) / step) + 2;
  gint16 *dst = g_new (gint16, capacity * (gsize) ch);
  gsize produced = 0;

  gdouble pos = self->pos;
  while (pos < limit && produced < capacity) {
    gint    base = (gint) floor (pos);
    gdouble frac = pos - base;

    /* Which stored phase, and how far between it and the next. */
    gdouble ph    = frac * RESAMPLER_PHASES;
    gint    ph_i  = (gint) ph;
    gdouble ph_f  = ph - ph_i;
    if (ph_i >= RESAMPLER_PHASES) { ph_i = RESAMPLER_PHASES - 1; ph_f = 1.0; }

    const gdouble *k0 = self->kernel[ph_i];
    const gdouble *k1 = self->kernel[ph_i + 1];

    for (gint c = 0; c < ch; c++) {
      gdouble acc = 0.0;
      for (gint t = 0; t < RESAMPLER_TAPS; t++) {
        gint idx = base - (RESAMPLER_HALF - 1) + t;
        if (idx < 0 || idx >= (gint) tmp_frames)
          continue;
        gdouble w = k0[t] + (k1[t] - k0[t]) * ph_f;
        acc += w * (gdouble) tmp[(gsize) idx * ch + c];
      }
      /* Round rather than truncate, and clamp: a steep kernel can overshoot
       * slightly past full scale on transients, and wrapping there would be an
       * audible click rather than the inaudible clip. */
      gdouble v = nearbyint (acc);
      dst[produced * (gsize) ch + c] = (gint16) CLAMP (v, -32768.0, 32767.0);
    }

    produced++;
    pos += step;
  }

  /* Retain the tail as history and rebase the position onto it. */
  gsize keep = MIN (tmp_frames, hist_frames);
  memcpy (self->history,
          tmp + (tmp_frames - keep) * (gsize) ch,
          sizeof (gint16) * keep * (gsize) ch);
  self->pos = pos - (gdouble) (tmp_frames - keep);

  if (produced == 0) {
    g_free (dst);
    return 0;
  }

  *out = dst;
  return produced;
}

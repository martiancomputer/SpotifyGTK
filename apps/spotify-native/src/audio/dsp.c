/*
 * dsp.c — Software graphic equaliser (RBJ peaking biquads).
 */

#include "dsp.h"
#include <math.h>
#include <string.h>

/* ISO 2/3-octave centres. Ten octave-spaced bands were coarse enough that a
 * single slider moved a whole octave at once; this is the next standard step
 * up and lands the handles where the ear expects them. */
const gint spotifygtk_eq_frequencies[SPOTIFYGTK_EQ_BANDS] = {
  25, 40, 63, 100, 160, 250, 400, 630,
  1000, 1600, 2500, 4000, 6300, 10000, 16000
};

/* Q must track the spacing: at 2/3-octave the octave-spaced sqrt(2) left the
 * bands overlapping heavily, so adjacent sliders fought each other and the
 * summed response bulged. Q = 2^(N/2)/(2^N - 1) with N = 2/3. */
#define EQ_Q       2.1450849718747373
#define EQ_MAX_CH  2

typedef struct {
  gdouble b0, b1, b2, a1, a2;   /* normalised coefficients (a0 = 1) */
} Biquad;

struct _SpotifyEq {
  gboolean enabled;
  gdouble  gains_db[SPOTIFYGTK_EQ_BANDS];

  /* Coefficients are recomputed when gains or sample rate change. */
  gint     coeff_rate;
  gdouble  coeff_gains[SPOTIFYGTK_EQ_BANDS];
  Biquad   biquad[SPOTIFYGTK_EQ_BANDS];

  /* Transposed direct-form-II state, per band per channel. */
  gdouble  z1[SPOTIFYGTK_EQ_BANDS][EQ_MAX_CH];
  gdouble  z2[SPOTIFYGTK_EQ_BANDS][EQ_MAX_CH];
  gboolean have_state;
};

SpotifyEq *
spotifygtk_eq_new (void)
{
  SpotifyEq *eq = g_new0 (SpotifyEq, 1);
  eq->coeff_rate = -1;
  return eq;
}

void
spotifygtk_eq_free (SpotifyEq *eq)
{
  g_free (eq);
}

void
spotifygtk_eq_set (SpotifyEq *eq, const gdouble *gains_db, gboolean enabled)
{
  if (!eq)
    return;
  eq->enabled = enabled;
  if (gains_db)
    memcpy (eq->gains_db, gains_db, sizeof eq->gains_db);
}

static gboolean
all_flat (const gdouble *gains_db)
{
  for (int i = 0; i < SPOTIFYGTK_EQ_BANDS; i++)
    if (fabs (gains_db[i]) > 0.05)
      return FALSE;
  return TRUE;
}

/* RBJ cookbook peaking EQ. */
static void
compute_biquad (Biquad *bq, gdouble freq, gdouble gain_db, gdouble rate)
{
  gdouble A     = pow (10.0, gain_db / 40.0);
  gdouble w0    = 2.0 * G_PI * freq / rate;
  gdouble cosw0 = cos (w0);
  gdouble alpha = sin (w0) / (2.0 * EQ_Q);

  gdouble a0 = 1.0 + alpha / A;
  bq->b0 = (1.0 + alpha * A) / a0;
  bq->b1 = (-2.0 * cosw0)    / a0;
  bq->b2 = (1.0 - alpha * A) / a0;
  bq->a1 = (-2.0 * cosw0)    / a0;
  bq->a2 = (1.0 - alpha / A) / a0;
}

static void
refresh_coeffs (SpotifyEq *eq, gint rate)
{
  gboolean rate_changed  = (rate != eq->coeff_rate);
  gboolean gains_changed = (memcmp (eq->coeff_gains, eq->gains_db,
                                    sizeof eq->gains_db) != 0);
  if (!rate_changed && !gains_changed)
    return;

  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++)
    compute_biquad (&eq->biquad[b], spotifygtk_eq_frequencies[b],
                    eq->gains_db[b], (gdouble) rate);

  eq->coeff_rate = rate;
  memcpy (eq->coeff_gains, eq->gains_db, sizeof eq->gains_db);

  /* Rate change invalidates the filter memory; a gain-only change does not,
   * so audio keeps flowing without a click when a slider moves. */
  if (rate_changed || !eq->have_state) {
    memset (eq->z1, 0, sizeof eq->z1);
    memset (eq->z2, 0, sizeof eq->z2);
    eq->have_state = TRUE;
  }
}

void
spotifygtk_eq_process (SpotifyEq *eq, gint16 *samples,
                       gsize n_frames, gint channels, gint sample_rate)
{
  if (!eq || !eq->enabled || !samples || n_frames == 0 || sample_rate <= 0)
    return;
  if (all_flat (eq->gains_db))
    return;

  gint ch = CLAMP (channels, 1, EQ_MAX_CH);
  refresh_coeffs (eq, sample_rate);

  for (gsize i = 0; i < n_frames; i++) {
    for (gint c = 0; c < ch; c++) {
      gdouble x = (gdouble) samples[i * channels + c];

      /* Cascade the bands; transposed DF-II per band. */
      for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++) {
        Biquad *bq = &eq->biquad[b];
        gdouble y = bq->b0 * x + eq->z1[b][c];
        eq->z1[b][c] = bq->b1 * x - bq->a1 * y + eq->z2[b][c];
        eq->z2[b][c] = bq->b2 * x - bq->a2 * y;
        x = y;
      }

      gint32 out = (gint32) lrint (x);
      samples[i * channels + c] = (gint16) CLAMP (out, G_MININT16, G_MAXINT16);
    }
  }
}


void
spotifygtk_eq_response_curve (const gdouble *gains_db, gint sample_rate,
                              const gdouble *freqs_hz, gdouble *out_db, gsize n)
{
  if (!gains_db || !freqs_hz || !out_db || sample_rate <= 0)
    return;

  /* Once for the whole curve, not once per point. */
  Biquad bq[SPOTIFYGTK_EQ_BANDS];
  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++)
    compute_biquad (&bq[b], spotifygtk_eq_frequencies[b], gains_db[b],
                    (gdouble) sample_rate);

  for (gsize i = 0; i < n; i++) {
    gdouble f = freqs_hz[i];
    if (f <= 0.0) { out_db[i] = 0.0; continue; }

    gdouble w     = 2.0 * G_PI * f / (gdouble) sample_rate;
    gdouble cosw  = cos (w);
    gdouble cos2w = cos (2.0 * w);

    /* Cascaded biquads multiply in magnitude. Accumulate the product and take
     * a single log at the end rather than one per band. */
    gdouble prod = 1.0;
    for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++) {
      gdouble num = bq[b].b0 * bq[b].b0 + bq[b].b1 * bq[b].b1 + bq[b].b2 * bq[b].b2
                  + 2.0 * (bq[b].b0 * bq[b].b1 + bq[b].b1 * bq[b].b2) * cosw
                  + 2.0 * bq[b].b0 * bq[b].b2 * cos2w;
      gdouble den = 1.0 + bq[b].a1 * bq[b].a1 + bq[b].a2 * bq[b].a2
                  + 2.0 * (bq[b].a1 + bq[b].a1 * bq[b].a2) * cosw
                  + 2.0 * bq[b].a2 * cos2w;
      if (den > 1e-20 && num > 1e-20)
        prod *= num / den;
    }
    out_db[i] = 10.0 * log10 (MAX (prod, 1e-20));
  }
}

gdouble
spotifygtk_eq_magnitude_db (const gdouble *gains_db, gdouble freq_hz, gint sample_rate)
{
  gdouble out = 0.0;
  spotifygtk_eq_response_curve (gains_db, sample_rate, &freq_hz, &out, 1);
  return out;
}

/*
 * test_resampler.c — Offline checks for the sample-rate converter.
 *
 * The resampler is pure PCM maths, so unlike the output backends it can be
 * verified without a sound card. These are the properties that matter: equal
 * rates must not touch the data at all, the frame count must follow the rate
 * ratio, a steady tone must survive with its level and frequency intact, and
 * DC must not drift (which is what catches an unnormalised kernel).
 */

#include <glib.h>
#include <math.h>
#include <string.h>

#include "audio/resampler.h"

#define CH 2

static gint16 *
make_sine (gsize frames, gdouble hz, gint rate, gdouble amp)
{
  gint16 *buf = g_new (gint16, frames * CH);
  for (gsize i = 0; i < frames; i++) {
    gdouble v = amp * sin (2.0 * G_PI * hz * (gdouble) i / (gdouble) rate);
    buf[i * CH + 0] = (gint16) lrint (v);
    buf[i * CH + 1] = (gint16) lrint (v);
  }
  return buf;
}

static gdouble
peak_of (const gint16 *b, gsize frames)
{
  gdouble p = 0.0;
  for (gsize i = 0; i < frames; i++)
    p = MAX (p, fabs ((gdouble) b[i * CH]));
  return p;
}

/* Equal rates must be a byte-exact copy, not a filtered near-copy: the whole
 * point of the passthrough default is that nothing touches the samples. */
static void
test_passthrough_is_exact (void)
{
  SpotifyResampler *r = spotifygtk_resampler_new (CH);
  spotifygtk_resampler_set_rates (r, 44100, 44100);
  g_assert_true (spotifygtk_resampler_is_passthrough (r));

  gsize n = 512;
  g_autofree gint16 *in = make_sine (n, 1000.0, 44100, 12000.0);
  gint16 *out = NULL;
  gsize got = spotifygtk_resampler_process (r, in, n, &out);

  g_assert_cmpuint (got, ==, n);
  g_assert_cmpint (memcmp (in, out, sizeof (gint16) * n * CH), ==, 0);

  g_free (out);
  spotifygtk_resampler_free (r);
}

/* Frame count tracks the ratio. Allow a small margin: the filter needs half a
 * kernel of history before it can emit, so the first block runs slightly short. */
static void
test_frame_count_follows_ratio (void)
{
  SpotifyResampler *r = spotifygtk_resampler_new (CH);
  spotifygtk_resampler_set_rates (r, 44100, 48000);
  g_assert_false (spotifygtk_resampler_is_passthrough (r));

  gsize n = 44100;                       /* one second */
  g_autofree gint16 *in = make_sine (n, 440.0, 44100, 10000.0);
  gint16 *out = NULL;
  gsize got = spotifygtk_resampler_process (r, in, n, &out);

  gdouble expected = n * (48000.0 / 44100.0);
  g_assert_cmpfloat (fabs ((gdouble) got - expected), <, 64.0);

  g_free (out);
  spotifygtk_resampler_free (r);
}

/* A steady tone must keep its amplitude. A badly normalised kernel shows up
 * here as a level shift, and a broken one as near-silence. */
static void
test_tone_level_preserved (void)
{
  SpotifyResampler *r = spotifygtk_resampler_new (CH);
  spotifygtk_resampler_set_rates (r, 44100, 48000);

  gsize n = 20000;
  const gdouble amp = 10000.0;
  g_autofree gint16 *in = make_sine (n, 1000.0, 44100, amp);
  gint16 *out = NULL;
  gsize got = spotifygtk_resampler_process (r, in, n, &out);
  g_assert_cmpuint (got, >, 0);

  /* Skip the leading transient before measuring. */
  gsize skip = MIN (got / 10, 512);
  gdouble p = peak_of (out + skip * CH, got - skip);
  g_assert_cmpfloat (p, >, amp * 0.95);
  g_assert_cmpfloat (p, <, amp * 1.05);

  g_free (out);
  spotifygtk_resampler_free (r);
}

/* Constant input must come out constant. This is the direct test of per-phase
 * kernel normalisation: without it the output ripples as the phase walks. */
static void
test_dc_is_preserved (void)
{
  SpotifyResampler *r = spotifygtk_resampler_new (CH);
  spotifygtk_resampler_set_rates (r, 44100, 48000);

  gsize n = 4096;
  g_autofree gint16 *in = g_new (gint16, n * CH);
  for (gsize i = 0; i < n * CH; i++)
    in[i] = 8000;

  gint16 *out = NULL;
  gsize got = spotifygtk_resampler_process (r, in, n, &out);
  g_assert_cmpuint (got, >, 0);

  for (gsize i = 512; i < got - 64; i++)
    g_assert_cmpfloat (fabs ((gdouble) out[i * CH] - 8000.0), <, 8.0);

  g_free (out);
  spotifygtk_resampler_free (r);
}

/* Downsampling must band-limit rather than fold: a tone above the new Nyquist
 * has to be attenuated, not mirrored back into the audible range. */
static void
test_downsample_rejects_above_nyquist (void)
{
  SpotifyResampler *r = spotifygtk_resampler_new (CH);
  spotifygtk_resampler_set_rates (r, 48000, 16000);   /* new Nyquist = 8 kHz */

  gsize n = 24000;
  const gdouble amp = 12000.0;
  g_autofree gint16 *in = make_sine (n, 11000.0, 48000, amp);  /* above it */
  gint16 *out = NULL;
  gsize got = spotifygtk_resampler_process (r, in, n, &out);
  g_assert_cmpuint (got, >, 0);

  gsize skip = MIN (got / 4, 1024);
  gdouble p = peak_of (out + skip * CH, got - skip);
  g_assert_cmpfloat (p, <, amp * 0.2);   /* strongly attenuated, not aliased */

  g_free (out);
  spotifygtk_resampler_free (r);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/resampler/passthrough-is-exact",   test_passthrough_is_exact);
  g_test_add_func ("/resampler/frame-count-ratio",      test_frame_count_follows_ratio);
  g_test_add_func ("/resampler/tone-level-preserved",   test_tone_level_preserved);
  g_test_add_func ("/resampler/dc-preserved",           test_dc_is_preserved);
  g_test_add_func ("/resampler/downsample-rejects-hf",  test_downsample_rejects_above_nyquist);
  return g_test_run ();
}

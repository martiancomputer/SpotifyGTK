/*
 * test_dsp.c — Equaliser sanity: a flat/disabled EQ must not touch the
 * signal, and an enabled band must actually change it. This is not a full
 * frequency-response verification — it guards the wiring and the "flat is a
 * no-op" fast path that keeps the EQ cheap when neutral.
 */

#include <glib.h>
#include <string.h>
#include <math.h>

#include "audio/dsp.h"

static void
fill_tone (gint16 *buf, gsize frames, gint ch, double freq, double rate)
{
  for (gsize i = 0; i < frames; i++) {
    gint16 v = (gint16) (12000.0 * sin (2.0 * G_PI * freq * i / rate));
    for (gint c = 0; c < ch; c++)
      buf[i * ch + c] = v;
  }
}

/* Disabled, or enabled-but-flat, must leave every sample byte-identical. */
static void
test_flat_is_noop (void)
{
  const gsize frames = 4096;
  gint16 a[frames * 2], b[frames * 2];
  fill_tone (a, frames, 2, 440.0, 44100.0);
  memcpy (b, a, sizeof a);

  SpotifyEq *eq = spotifygtk_eq_new ();
  gdouble flat[SPOTIFYGTK_EQ_BANDS] = { 0 };

  spotifygtk_eq_set (eq, flat, FALSE);            /* disabled */
  spotifygtk_eq_process (eq, b, frames, 2, 44100);
  g_assert_cmpint (memcmp (a, b, sizeof a), ==, 0);

  spotifygtk_eq_set (eq, flat, TRUE);             /* enabled but all 0 dB */
  spotifygtk_eq_process (eq, b, frames, 2, 44100);
  g_assert_cmpint (memcmp (a, b, sizeof a), ==, 0);

  spotifygtk_eq_free (eq);
}

/* A large boost on the band nearest the tone must raise its amplitude; a cut
 * must lower it. Uses a 1 kHz tone against the 1 kHz band. */
static void
test_band_changes_amplitude (void)
{
  const gsize frames = 8192;
  const double rate = 44100.0, freq = 1000.0;

  gint16 base[frames], boosted[frames], cut[frames];
  fill_tone (base, frames, 1, freq, rate);
  memcpy (boosted, base, sizeof base);
  memcpy (cut, base, sizeof base);

  gdouble up[SPOTIFYGTK_EQ_BANDS]   = { 0 };
  gdouble down[SPOTIFYGTK_EQ_BANDS] = { 0 };

  /* Look the band up rather than hardcoding an index: the band count and
   * spacing have changed once already (10 octave bands -> 15 at 2/3 octave),
   * and a stale literal silently tested the wrong band instead of failing. */
  gint band = 0;
  for (int i = 0; i < SPOTIFYGTK_EQ_BANDS; i++)
    if (spotifygtk_eq_frequencies[i] == (gint) freq) { band = i; break; }
  up[band]   = 12.0;    /* the 1 kHz band */
  down[band] = -12.0;

  SpotifyEq *eq_up = spotifygtk_eq_new ();
  spotifygtk_eq_set (eq_up, up, TRUE);
  spotifygtk_eq_process (eq_up, boosted, frames, 1, (gint) rate);
  spotifygtk_eq_free (eq_up);

  SpotifyEq *eq_dn = spotifygtk_eq_new ();
  spotifygtk_eq_set (eq_dn, down, TRUE);
  spotifygtk_eq_process (eq_dn, cut, frames, 1, (gint) rate);
  spotifygtk_eq_free (eq_dn);

  /* Peak amplitude over the settled tail (skip filter warm-up). */
  gint base_pk = 0, boost_pk = 0, cut_pk = 0;
  for (gsize i = 2000; i < frames; i++) {
    base_pk  = MAX (base_pk,  abs (base[i]));
    boost_pk = MAX (boost_pk, abs (boosted[i]));
    cut_pk   = MAX (cut_pk,   abs (cut[i]));
  }

  g_assert_cmpint (boost_pk, >, base_pk);
  g_assert_cmpint (cut_pk,   <, base_pk);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/dsp/flat-is-noop", test_flat_is_noop);
  g_test_add_func ("/dsp/band-changes-amplitude", test_band_changes_amplitude);
  return g_test_run ();
}

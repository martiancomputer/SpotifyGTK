/*
 * dsp.h — Software graphic equaliser.
 *
 * A ten-band graphic EQ applied to decoded PCM in the audio worker, between
 * the volume stage and the output. Each band is an RBJ peaking biquad; the
 * bands cascade, per channel, with the filter memory carried across buffers
 * so there are no discontinuities at buffer boundaries.
 *
 * Standalone and pure C (no GTK, no settings dependency) so the engine does
 * not pull in the UI layer: the caller pushes gains in, the same way volume
 * is pushed to the engine control.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_EQ_BANDS 15

/* ISO-ish octave centres, Hz. Exposed so the UI can label its sliders with
 * the same frequencies the filters actually use. */
extern const gint spotifygtk_eq_frequencies[SPOTIFYGTK_EQ_BANDS];

typedef struct _SpotifyEq SpotifyEq;

SpotifyEq *spotifygtk_eq_new  (void);
void       spotifygtk_eq_free (SpotifyEq *eq);

/*
 * Set the band gains in dB (each roughly -12..+12) and whether the EQ is on.
 * Cheap to call every buffer; coefficients are only recomputed when a value
 * actually changed. `gains` is an array of SPOTIFYGTK_EQ_BANDS.
 */
void spotifygtk_eq_set (SpotifyEq *eq, const gdouble *gains_db, gboolean enabled);

/*
 * Filter `samples` (interleaved 16-bit PCM) in place. `sample_rate` may
 * change between calls (different track); coefficients follow it. A no-op
 * when disabled or when every gain is flat, so leaving the EQ on but neutral
 * costs almost nothing.
 */
void spotifygtk_eq_process (SpotifyEq *eq, gint16 *samples,
                            gsize n_frames, gint channels, gint sample_rate);

/*
 * Combined magnitude response of the whole band cascade at `freq_hz`, in dB.
 * This is the same coefficient maths the audio path uses, so the curve the UI
 * draws is the filter that actually runs -- including how neighbouring bands
 * overlap and sum -- rather than a pretty interpolation through the handles.
 */
gdouble spotifygtk_eq_magnitude_db (const gdouble *gains_db,
                                    gdouble freq_hz, gint sample_rate);

/*
 * Same response, evaluated at `n` frequencies at once. The band coefficients
 * are computed once for the whole call rather than once per frequency, which
 * is the difference between a UI that redraws smoothly and one that does
 * thousands of pow/cos per frame. `out_db` must have room for `n`.
 */
void spotifygtk_eq_response_curve (const gdouble *gains_db, gint sample_rate,
                                   const gdouble *freqs_hz, gdouble *out_db,
                                   gsize n);

G_END_DECLS

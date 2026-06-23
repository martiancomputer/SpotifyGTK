/*
 * output_wasapi.c — Windows audio output (WASAPI) — PORT PHASE STUB.
 *
 * Not built yet (not in audio/meson.build) — this file exists so the
 * Windows port has a designated landing spot that mirrors the
 * structure of output_pulse.c / output_alsa.c exactly. When the
 * Windows port begins:
 *
 *   1. Add `#ifdef _WIN32` conditional sources in audio/meson.build
 *   2. Implement using IAudioClient + IAudioRenderClient (shared mode
 *      is fine for a music player — exclusive mode adds complexity
 *      for no benefit here)
 *   3. Same vtable shape as every other backend — player.c and
 *      decoder.c need zero changes
 *
 * Output abstraction was designed for this from day one specifically
 * so the Windows port doesn't touch anything above this file.
 */

#include "config.h"
#include "output.h"

#ifdef _WIN32
/* #include <audioclient.h>
 * #include <mmdeviceapi.h>
 * Real implementation goes here when the Windows port phase begins. */
#endif

gboolean
output_wasapi_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{
  (void) self; (void) rate; (void) channels;
  return FALSE;  /* not implemented — Linux ships first */
}

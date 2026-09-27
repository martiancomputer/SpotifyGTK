#include "config.h"
#include <glib.h>
#include <string.h>

#include "audio/output.h"

/* The dispatcher is linked without real devices so container conversion is
 * testable on build machines with no audio server. */
gboolean output_pulse_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{ (void) self; (void) rate; (void) channels; return FALSE; }
gboolean output_alsa_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{ (void) self; (void) rate; (void) channels; return FALSE; }
#if HAVE_PIPEWIRE
gboolean output_pipewire_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{ (void) self; (void) rate; (void) channels; return FALSE; }
#endif
#ifdef G_OS_WIN32
gboolean output_wasapi_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{ (void) self; (void) rate; (void) channels; return FALSE; }
#endif

static guint8 captured[32];

static gsize
capture_write (SpotifyAudioOutput *self, const void *samples, gsize frames)
{
  gsize bytes = frames * (gsize) self->channels * (gsize) self->format_bits / 8;
  g_assert_cmpuint (bytes, <=, sizeof captured);
  memcpy (captured, samples, bytes);
  return frames;
}

static void
test_s16_container_widening (void)
{
  const AudioBackendVtable vtable = { .write = capture_write };
  const gint16 samples[] = { 0x1234, -1, G_MININT16 };
  SpotifyAudioOutput out = { .vtable = &vtable, .channels = 1 };

  out.format_bits = 16;
  g_assert_cmpuint (spotifygtk_output_write (&out, samples, 3), ==, 3);
  g_assert_cmpint (memcmp (captured, samples, sizeof samples), ==, 0);

  out.format_bits = 24;
  const guint8 expected24[] = { 0x00, 0x34, 0x12,
                                0x00, 0xff, 0xff,
                                0x00, 0x00, 0x80 };
  g_assert_cmpuint (spotifygtk_output_write (&out, samples, 3), ==, 3);
  g_assert_cmpint (memcmp (captured, expected24, sizeof expected24), ==, 0);

  out.format_bits = 32;
  const guint8 expected32[] = { 0x00, 0x00, 0x34, 0x12,
                                0x00, 0x00, 0xff, 0xff,
                                0x00, 0x00, 0x00, 0x80 };
  g_assert_cmpuint (spotifygtk_output_write (&out, samples, 3), ==, 3);
  g_assert_cmpint (memcmp (captured, expected32, sizeof expected32), ==, 0);
  g_free (out.pack_buffer);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/output/s16-container-widening", test_s16_container_widening);
  return g_test_run ();
}

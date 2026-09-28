#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "audio/local_playback.h"
#include "audio/sink.h"

/* A counting sink proves that the local adapter emits bounded interleaved
 * PCM frames without touching the user's audio device. Production uses the
 * real sink with exactly these entry points. */
struct _SpotifyAudioSink { guint64 frames; guint pushes; gboolean ended; };
struct _SpotifyNativeEngineControl { guint64 seq; };
static SpotifyAudioSink sink;

SpotifyAudioSink *spotifygtk_audio_sink_get (void) { return &sink; }
guint64
spotifygtk_audio_sink_begin_track (SpotifyAudioSink *self,
                                   SpotifyNativeEngineControl *control,
                                   GCancellable *cancel)
{
  (void) self; (void) control; (void) cancel;
  return 1;
}
gboolean
spotifygtk_audio_sink_push (SpotifyAudioSink *self, guint64 seq, PcmFrame *frame)
{
  g_assert_cmpuint (seq, ==, 1);
  g_assert_cmpint (frame->channels, ==, 2);
  g_assert_cmpint (frame->sample_rate, ==, 8000);
  g_assert_cmpuint (frame->n_frames, >, 0);
  g_assert_cmpuint (frame->n_frames, <=, 65536);
  self->frames += frame->n_frames;
  self->pushes++;
  pcm_frame_free (frame);
  return TRUE;
}
void spotifygtk_audio_sink_end_track (SpotifyAudioSink *self, guint64 seq)
{ g_assert_cmpuint (seq, ==, 1); self->ended = TRUE; }
void spotifygtk_audio_sink_flush (SpotifyAudioSink *self, guint64 seq)
{ (void) self; (void) seq; }
void spotifygtk_audio_sink_set_position (SpotifyAudioSink *self, guint64 seq,
                                         guint64 frames)
{ (void) self; (void) seq; (void) frames; }
gint spotifygtk_audio_sink_device_rate (SpotifyAudioSink *self)
{ (void) self; return 8000; }

void spotifygtk_native_engine_control_set_sink_seq (
  SpotifyNativeEngineControl *control, guint64 seq) { control->seq = seq; }
gboolean spotifygtk_native_engine_control_wait (
  SpotifyNativeEngineControl *control, GCancellable *cancel)
{ (void) control; return !g_cancellable_is_cancelled (cancel); }
gboolean spotifygtk_native_engine_control_seek_pending (
  SpotifyNativeEngineControl *control) { (void) control; return FALSE; }
gboolean spotifygtk_native_engine_control_take_seek (
  SpotifyNativeEngineControl *control, gint64 *position)
{ (void) control; (void) position; return FALSE; }
void pcm_frame_free (PcmFrame *frame)
{ g_free (frame->samples); g_free (frame); }

static void
put_le16 (guchar *p, guint16 value)
{ p[0] = value & 0xff; p[1] = value >> 8; }
static void
put_le32 (guchar *p, guint32 value)
{ for (guint i = 0; i < 4; i++) p[i] = value >> (8 * i); }

static void
test_pcm_adapter (void)
{
  g_autofree gchar *dir = g_dir_make_tmp ("spotifygtk-playback-test-XXXXXX", NULL);
  g_assert_nonnull (dir);
  g_autofree gchar *path = g_build_filename (dir, "sample.wav", NULL);
  g_autofree guchar *bytes = g_malloc0 (44 + 16000);
  memcpy (bytes, "RIFF", 4);
  put_le32 (bytes + 4, 36 + 16000);
  memcpy (bytes + 8, "WAVEfmt ", 8);
  put_le32 (bytes + 16, 16);
  put_le16 (bytes + 20, 1);
  put_le16 (bytes + 22, 1);
  put_le32 (bytes + 24, 8000);
  put_le32 (bytes + 28, 16000);
  put_le16 (bytes + 32, 2);
  put_le16 (bytes + 34, 16);
  memcpy (bytes + 36, "data", 4);
  put_le32 (bytes + 40, 16000);
  g_assert_true (g_file_set_contents (path, (const gchar *) bytes,
                                      44 + 16000, NULL));
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  SpotifyNativeEngineControl control = { 0 };
  memset (&sink, 0, sizeof sink);
  g_assert_true (spotifygtk_local_playback_run (path, cancel, NULL, NULL,
                                                 &control));
  g_assert_cmpuint (control.seq, ==, 1);
  g_assert_cmpuint (sink.frames, ==, 8000);
  g_assert_cmpuint (sink.pushes, >, 0);
  g_assert_true (sink.ended);
  g_assert_cmpint (g_remove (path), ==, 0);
  g_assert_cmpint (g_rmdir (dir), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/local/pcm-adapter", test_pcm_adapter);
  return g_test_run ();
}

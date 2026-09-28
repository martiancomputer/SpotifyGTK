#include "config.h"
#include "local_playback.h"
#include "sink.h"

#if HAVE_LOCAL_AV
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>

/* This is deliberately a 16-bit adapter. The existing sink, DSP and output
 * pipeline are 16-bit internally; a wider device container does not restore
 * the precision of a 24-bit FLAC source. */
static gboolean
queue_decoded (SwrContext *resampler, AVFrame *decoded,
               SpotifyAudioSink *sink, guint64 seq,
               SpotifyNativeEngineControl *control,
               GCancellable *cancel, gint rate)
{
  if (decoded->nb_samples <= 0 || decoded->nb_samples > 32768)
    return FALSE;
  gint n = swr_get_out_samples (resampler, decoded->nb_samples);
  if (n <= 0 || n > 65536)
    return FALSE;
  PcmFrame *pcm = g_new0 (PcmFrame, 1);
  pcm->samples = g_new (gint16, (gsize) n * 2);
  pcm->channels = 2;
  pcm->sample_rate = rate;
  guint8 *out[] = { (guint8 *) pcm->samples };
  gint converted = swr_convert (resampler, out, n,
                                 (const guint8 **) decoded->extended_data,
                                 decoded->nb_samples);
  if (converted < 0) {
    pcm_frame_free (pcm);
    return FALSE;
  }
  pcm->n_frames = converted;
  if (converted == 0) {
    pcm_frame_free (pcm);
    return TRUE;
  }
  if (g_cancellable_is_cancelled (cancel) ||
      spotifygtk_native_engine_control_seek_pending (control)) {
    pcm_frame_free (pcm);
    return !g_cancellable_is_cancelled (cancel);
  }
  /* push() consumes the frame even when it rejects it. */
  return spotifygtk_audio_sink_push (sink, seq, pcm);
}
#endif

gboolean
spotifygtk_local_playback_run (const gchar *path, GCancellable *cancel,
                               SpotifyNativeEngineProgressFunc progress,
                               gpointer progress_data,
                               SpotifyNativeEngineControl *control)
{
#if !HAVE_LOCAL_AV
  (void) path; (void) cancel; (void) progress;
  (void) progress_data; (void) control;
  return FALSE;
#else
  if (!path || !g_path_is_absolute (path) ||
      g_cancellable_is_cancelled (cancel)) return FALSE;
  if (progress) progress (SPOTIFYGTK_ENGINE_BUFFERING,
                          "Opening local audio…", progress_data);
  AVFormatContext *format = NULL;
  AVCodecContext *decoder = NULL;
  SwrContext *resampler = NULL;
  AVPacket *packet = NULL;
  AVFrame *frame = NULL;
  guint64 seq = 0;
  gboolean success = FALSE, started = FALSE;
  SpotifyAudioSink *sink = spotifygtk_audio_sink_get ();
  if (avformat_open_input (&format, path, NULL, NULL) < 0 ||
      avformat_find_stream_info (format, NULL) < 0) goto done;
  int index = av_find_best_stream (format, AVMEDIA_TYPE_AUDIO, -1, -1,
                                   NULL, 0);
  if (index < 0) goto done;
  AVStream *stream = format->streams[index];
  const AVCodec *codec = avcodec_find_decoder (stream->codecpar->codec_id);
  if (!codec) goto done;
  decoder = avcodec_alloc_context3 (codec);
  if (!decoder || avcodec_parameters_to_context (decoder, stream->codecpar) < 0 ||
      avcodec_open2 (decoder, codec, NULL) < 0 ||
      decoder->sample_rate < 8000 || decoder->sample_rate > 384000 ||
      decoder->ch_layout.nb_channels < 1 ||
      decoder->ch_layout.nb_channels > 8) goto done;
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  if (swr_alloc_set_opts2 (&resampler, &stereo, AV_SAMPLE_FMT_S16,
                           decoder->sample_rate, &decoder->ch_layout,
                           decoder->sample_fmt, decoder->sample_rate,
                           0, NULL) < 0 || !resampler ||
      swr_init (resampler) < 0) goto done;
  packet = av_packet_alloc ();
  frame = av_frame_alloc ();
  if (!packet || !frame) goto done;

  seq = spotifygtk_audio_sink_begin_track (sink, control, cancel);
  spotifygtk_native_engine_control_set_sink_seq (control, seq);
  if (!seq) goto done;
  gboolean eof = FALSE;
  while (!g_cancellable_is_cancelled (cancel)) {
    if (!spotifygtk_native_engine_control_wait (control, cancel)) break;
    gint64 seek_ms;
    if (spotifygtk_native_engine_control_take_seek (control, &seek_ms)) {
      gint64 timestamp = av_rescale_q (MAX (seek_ms, 0),
                                      (AVRational) { 1, 1000 }, stream->time_base);
      if (av_seek_frame (format, index, timestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers (decoder);
        swr_close (resampler);
        if (swr_init (resampler) < 0) break;
        spotifygtk_audio_sink_flush (sink, seq);
        gint device_rate = spotifygtk_audio_sink_device_rate (sink);
        spotifygtk_audio_sink_set_position (sink, seq,
          (guint64) MAX (seek_ms, 0) * (device_rate > 0 ? device_rate :
                                        decoder->sample_rate) / 1000);
        eof = FALSE;
      }
    }
    int received = avcodec_receive_frame (decoder, frame);
    if (received == 0) {
      gboolean queued = queue_decoded (resampler, frame, sink, seq, control,
                                       cancel, decoder->sample_rate);
      av_frame_unref (frame);
      if (!queued) break;
      if (!started) {
        started = TRUE;
        if (progress) progress (SPOTIFYGTK_ENGINE_PLAYING,
                                "Playing local audio.", progress_data);
      }
      continue;
    }
    if (received == AVERROR_EOF) { success = started; break; }
    if (received != AVERROR (EAGAIN)) break;
    if (eof) {
      if (avcodec_send_packet (decoder, NULL) < 0) break;
      continue;
    }
    gint read = av_read_frame (format, packet);
    if (read == AVERROR_EOF) {
      eof = TRUE;
      if (avcodec_send_packet (decoder, NULL) < 0) break;
      continue;
    }
    if (read < 0) break;
    if (packet->stream_index == index &&
        avcodec_send_packet (decoder, packet) < 0) {
      av_packet_unref (packet);
      break;
    }
    av_packet_unref (packet);
  }
done:
  if (seq) spotifygtk_audio_sink_end_track (sink, seq);
  av_frame_free (&frame);
  av_packet_free (&packet);
  swr_free (&resampler);
  avcodec_free_context (&decoder);
  avformat_close_input (&format);
  return success && !g_cancellable_is_cancelled (cancel);
#endif
}

/*
 * sink.c — see sink.h.
 */

#include "sink.h"

#include "output.h"
#include "resampler.h"

/*
 * Per-track buffering ceiling.
 *
 * Per track rather than overall, because the whole point is to let the next
 * track buffer while the current one plays -- a single shared limit would let
 * a prefetching decoder starve the track actually being heard.
 *
 * Roughly ten seconds at 44.1kHz. Enough to ride out a slow CDN range, short
 * enough that a skip does not have to throw much away.
 */
#define SINK_TRACK_MAX_FRAMES 441000u

/* One track's worth of audio, waiting its turn. */
typedef struct {
  guint64      seq;
  GQueue      *frames;         /* PcmFrame*, in decode order */
  guint64      queued;         /* frames still to be written */
  gboolean     ended;          /* producer has pushed its last frame */
  gboolean     started;        /* at least one frame has reached the device */
  gboolean     cancelled;      /* flushed away or abandoned */

  SpotifyNativeEngineControl *control;   /* referenced */
  GCancellable *cancellable;             /* referenced, may be NULL */

  guint64      written;        /* device frames written for this track */
  gint         stream_rate;    /* rate its PCM decoded to */
  SpotifyResampler *resampler; /* only when stream_rate != device rate */
} SinkTrack;

struct _SpotifyAudioSink {
  GMutex   lock;
  GCond    cond;               /* frame available, or space freed */

  GQueue  *tracks;             /* SinkTrack*, play order */
  guint64  next_seq;

  GThread *writer;
  gboolean running;
  gboolean failed;

  SpotifyAudioOutput *output;
  gint     device_rate;
  gint     device_channels;
};

static SpotifyAudioSink *the_sink = NULL;

static void
sink_track_free (SinkTrack *t)
{
  if (!t)
    return;
  if (t->frames) {
    PcmFrame *f;
    while ((f = g_queue_pop_head (t->frames)) != NULL)
      pcm_frame_free (f);
    g_queue_free (t->frames);
  }
  spotifygtk_resampler_free (t->resampler);
  g_clear_object (&t->cancellable);
  /* The reference taken in begin_track; the producer's own is separate. */
  spotifygtk_native_engine_control_free (t->control);
  g_free (t);
}

/* Caller holds the lock. */
static SinkTrack *
find_track (SpotifyAudioSink *self, guint64 seq)
{
  for (GList *l = self->tracks->head; l; l = l->next) {
    SinkTrack *t = l->data;
    if (t->seq == seq)
      return t;
  }
  return NULL;
}

/*
 * The track currently being written: the head of the queue, skipping any that
 * are finished or abandoned. Caller holds the lock.
 */
static SinkTrack *
head_track (SpotifyAudioSink *self)
{
  while (self->tracks->head) {
    SinkTrack *t = self->tracks->head->data;

    /*
     * A cancelled track is abandoned outright, buffered audio included.
     *
     * Cancellation was only checked when pushing, which stopped new frames but
     * left everything already queued to play out -- so choosing another track
     * kept the old one sounding for as long as the buffer lasted. The buffer
     * is the whole point of the handover, so that is up to ten seconds of the
     * wrong song.
     */
    if (!t->cancelled && t->cancellable && g_cancellable_is_cancelled (t->cancellable)) {
      PcmFrame *f;
      while ((f = g_queue_pop_head (t->frames)) != NULL)
        pcm_frame_free (f);
      t->queued = 0;
      t->cancelled = TRUE;
      g_cond_broadcast (&self->cond);
    }

    gboolean spent = (t->cancelled || t->ended) && g_queue_is_empty (t->frames);
    if (!spent)
      return t;

    g_queue_pop_head (self->tracks);
    if (t->started)
      g_message ("sink: finished track %" G_GUINT64_FORMAT
                 " (%" G_GUINT64_FORMAT " device frames)", t->seq, t->written);
    sink_track_free (t);
  }
  return NULL;
}

/*
 * Open the device once, from the first frame that reaches it.
 *
 * The rate is chosen here and then kept: every later track is resampled to it
 * rather than reopening, because reopening is the gap. Caller holds the lock;
 * the open itself is done unlocked since it can block.
 */
static gboolean
ensure_output (SpotifyAudioSink *self, const PcmFrame *frame, gint requested_rate)
{
  if (self->output)
    return TRUE;
  if (self->failed)
    return FALSE;

  gint rate = (requested_rate > 0) ? requested_rate : frame->sample_rate;
  gint channels = frame->channels;

  g_mutex_unlock (&self->lock);
  SpotifyAudioOutput *out = spotifygtk_output_open (rate, channels);
  g_mutex_lock (&self->lock);

  if (!out) {
    g_warning ("sink: no PCM output backend is available");
    self->failed = TRUE;
    g_cond_broadcast (&self->cond);
    return FALSE;
  }

  /* Another thread cannot have opened one -- there is a single writer -- but
   * the unlock above means the sink could have been shut down meanwhile. */
  if (!self->running) {
    spotifygtk_output_close (out);
    return FALSE;
  }

  self->output = out;
  self->device_rate = rate;
  self->device_channels = channels;
  g_message ("sink: device open at %d Hz, %d channel(s) on %s; it stays open "
             "across tracks", rate, channels,
             spotifygtk_output_backend_name (out->kind));
  return TRUE;
}

/* Convert one frame to the device rate if it did not decode to it. Returns the
 * buffer to write and sets *out_frames; *scratch owns any allocation. */
static const gint16 *
to_device_rate (SinkTrack *t, gint device_rate, PcmFrame *frame,
                gsize *out_frames, gint16 **scratch)
{
  *scratch = NULL;

  if (frame->sample_rate == device_rate) {
    *out_frames = frame->n_frames;
    return frame->samples;
  }

  if (!t->resampler || t->stream_rate != frame->sample_rate) {
    spotifygtk_resampler_free (t->resampler);
    t->resampler = spotifygtk_resampler_new (frame->channels);
    spotifygtk_resampler_set_rates (t->resampler, frame->sample_rate, device_rate);
    t->stream_rate = frame->sample_rate;
    g_message ("sink: track %" G_GUINT64_FORMAT " resampling %d Hz -> %d Hz to "
               "keep the device open", t->seq, frame->sample_rate, device_rate);
  }

  *out_frames = spotifygtk_resampler_process (t->resampler, frame->samples,
                                              frame->n_frames, scratch);
  return *scratch;
}

static gpointer
sink_writer (gpointer user_data)
{
  SpotifyAudioSink *self = user_data;

  g_mutex_lock (&self->lock);
  while (self->running) {
    SinkTrack *t = head_track (self);

    if (!t || g_queue_is_empty (t->frames)) {
      /*
       * Nothing to write. Deliberately does NOT drain the device: a drain here
       * would block until the buffer emptied, which is precisely the silence
       * this design removes. The device is left running with whatever it still
       * holds, and the next frame lands behind it.
       */
      g_cond_wait (&self->cond, &self->lock);
      continue;
    }

    if (self->failed) {
      PcmFrame *f = g_queue_pop_head (t->frames);
      t->queued -= f->n_frames;
      pcm_frame_free (f);
      g_cond_broadcast (&self->cond);
      continue;
    }

    gint requested = spotifygtk_native_engine_control_get_output_rate (t->control);
    PcmFrame *frame = g_queue_peek_head (t->frames);
    if (!ensure_output (self, frame, requested))
      continue;

    /* head_track may have changed while ensure_output had the lock dropped. */
    if (head_track (self) != t)
      continue;

    frame = g_queue_pop_head (t->frames);
    t->queued -= frame->n_frames;
    g_cond_broadcast (&self->cond);          /* a producer may be waiting on space */

    SpotifyNativeEngineControl *control = spotifygtk_native_engine_control_ref (t->control);
    GCancellable *cancel = t->cancellable ? g_object_ref (t->cancellable) : NULL;
    gint device_rate = self->device_rate;
    guint64 seq = t->seq;

    g_autofree gint16 *scratch = NULL;
    gsize n_frames = 0;
    const gint16 *pcm = to_device_rate (t, device_rate, frame, &n_frames, &scratch);

    if (n_frames == 0) {                     /* too short to yield a block yet */
      pcm_frame_free (frame);
      spotifygtk_native_engine_control_free (control);
      g_clear_object (&cancel);
      continue;
    }

    t->started = TRUE;
    t->written += n_frames;
    guint64 written_total = t->written;

    g_mutex_unlock (&self->lock);

    /* The pause gate and the device write happen unlocked: both can block for
     * a long time, and holding the lock through them would stop producers from
     * queueing the next track. */
    gboolean go = spotifygtk_native_engine_control_wait (control, cancel);
    gsize wrote = 0;
    if (go) {
      spotifygtk_native_engine_control_apply_volume (control, (gint16 *) pcm,
                                                        n_frames, frame->channels);
      spotifygtk_native_engine_control_apply_eq (control, (gint16 *) pcm,
                                                    n_frames, frame->channels,
                                                    device_rate);
      wrote = spotifygtk_output_write (self->output, pcm, n_frames);
      if (wrote == n_frames)
        spotifygtk_native_engine_control_report_position (control, written_total,
                                                          device_rate);
    }

    pcm_frame_free (frame);
    spotifygtk_native_engine_control_free (control);
    g_clear_object (&cancel);

    g_mutex_lock (&self->lock);
    if (go && wrote != n_frames) {
      g_warning ("sink: device took %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT
                 " frames on track %" G_GUINT64_FORMAT, wrote, n_frames, seq);
      self->failed = TRUE;
      g_cond_broadcast (&self->cond);
    }
  }

  /* Shutting down: this is the one place a drain belongs. */
  if (self->output) {
    g_mutex_unlock (&self->lock);
    spotifygtk_output_drain (self->output);
    spotifygtk_output_close (self->output);
    g_mutex_lock (&self->lock);
    self->output = NULL;
  }

  SinkTrack *t;
  while ((t = g_queue_pop_head (self->tracks)) != NULL)
    sink_track_free (t);

  g_mutex_unlock (&self->lock);
  return NULL;
}

SpotifyAudioSink *
spotifygtk_audio_sink_get (void)
{
  static gsize once = 0;
  if (g_once_init_enter (&once)) {
    SpotifyAudioSink *self = g_new0 (SpotifyAudioSink, 1);
    g_mutex_init (&self->lock);
    g_cond_init (&self->cond);
    self->tracks  = g_queue_new ();
    self->running = TRUE;
    self->writer  = g_thread_new ("spotify-sink", sink_writer, self);
    the_sink = self;
    g_once_init_leave (&once, 1);
  }
  return the_sink;
}

guint64
spotifygtk_audio_sink_begin_track (SpotifyAudioSink *self,
                                   SpotifyNativeEngineControl *control,
                                   GCancellable *cancellable)
{
  g_return_val_if_fail (self != NULL, 0);

  SinkTrack *t = g_new0 (SinkTrack, 1);
  t->frames      = g_queue_new ();
  t->control     = spotifygtk_native_engine_control_ref (control);
  t->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  g_mutex_lock (&self->lock);
  t->seq = ++self->next_seq;
  g_queue_push_tail (self->tracks, t);
  guint64 seq = t->seq;
  guint depth = g_queue_get_length (self->tracks);
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);

  g_message ("sink: track %" G_GUINT64_FORMAT " queued (%u in the play order)",
             seq, depth);
  return seq;
}

gboolean
spotifygtk_audio_sink_push (SpotifyAudioSink *self, guint64 seq, PcmFrame *frame)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (frame != NULL, FALSE);

  g_mutex_lock (&self->lock);

  SinkTrack *t = find_track (self, seq);
  while (t && !t->cancelled && !self->failed &&
         t->queued >= SINK_TRACK_MAX_FRAMES &&
         !(t->cancellable && g_cancellable_is_cancelled (t->cancellable))) {
    /* Timed, so a producer cannot be left waiting on a broadcast it missed. */
    g_cond_wait_until (&self->cond, &self->lock,
                       g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND);
    t = find_track (self, seq);
  }

  gboolean rejected = !t || t->cancelled || self->failed ||
                      (t->cancellable && g_cancellable_is_cancelled (t->cancellable));
  if (!rejected) {
    g_queue_push_tail (t->frames, frame);
    t->queued += frame->n_frames;
    g_cond_broadcast (&self->cond);
  }
  g_mutex_unlock (&self->lock);

  if (rejected) {
    pcm_frame_free (frame);
    return FALSE;
  }
  return TRUE;
}

void
spotifygtk_audio_sink_end_track (SpotifyAudioSink *self, guint64 seq)
{
  g_return_if_fail (self != NULL);

  g_mutex_lock (&self->lock);
  SinkTrack *t = find_track (self, seq);
  if (t)
    t->ended = TRUE;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
}

void
spotifygtk_audio_sink_flush (SpotifyAudioSink *self, guint64 seq)
{
  g_return_if_fail (self != NULL);

  g_mutex_lock (&self->lock);
  SinkTrack *t = find_track (self, seq);
  if (t) {
    PcmFrame *f;
    while ((f = g_queue_pop_head (t->frames)) != NULL)
      pcm_frame_free (f);
    t->queued = 0;
  }
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
}

void
spotifygtk_audio_sink_set_position (SpotifyAudioSink *self, guint64 seq,
                                    guint64 device_frames)
{
  g_return_if_fail (self != NULL);

  g_mutex_lock (&self->lock);
  SinkTrack *t = find_track (self, seq);
  if (t)
    t->written = device_frames;
  g_mutex_unlock (&self->lock);
}

guint64
spotifygtk_audio_sink_queued_frames (SpotifyAudioSink *self, guint64 seq)
{
  g_return_val_if_fail (self != NULL, 0);

  g_mutex_lock (&self->lock);
  SinkTrack *t = find_track (self, seq);
  guint64 n = t ? t->queued : 0;
  g_mutex_unlock (&self->lock);
  return n;
}

guint64
spotifygtk_audio_sink_current_seq (SpotifyAudioSink *self)
{
  g_return_val_if_fail (self != NULL, 0);

  g_mutex_lock (&self->lock);
  /* The head that has actually been heard. A slot claimed but not yet started
   * is the next track buffering, not the one playing. */
  guint64 seq = 0;
  for (GList *l = self->tracks->head; l; l = l->next) {
    SinkTrack *t = l->data;
    if (t->started && !t->cancelled) { seq = t->seq; break; }
  }
  g_mutex_unlock (&self->lock);
  return seq;
}

gboolean
spotifygtk_audio_sink_track_done (SpotifyAudioSink *self, guint64 seq)
{
  g_return_val_if_fail (self != NULL, TRUE);

  g_mutex_lock (&self->lock);
  gboolean done = (find_track (self, seq) == NULL);
  g_mutex_unlock (&self->lock);
  return done;
}

gboolean
spotifygtk_audio_sink_failed (SpotifyAudioSink *self)
{
  g_return_val_if_fail (self != NULL, TRUE);

  g_mutex_lock (&self->lock);
  gboolean failed = self->failed;
  g_mutex_unlock (&self->lock);
  return failed;
}

gint
spotifygtk_audio_sink_device_rate (SpotifyAudioSink *self)
{
  g_return_val_if_fail (self != NULL, 0);

  g_mutex_lock (&self->lock);
  gint rate = self->device_rate;
  g_mutex_unlock (&self->lock);
  return rate;
}

void
spotifygtk_audio_sink_shutdown (SpotifyAudioSink *self)
{
  if (!self)
    return;

  g_mutex_lock (&self->lock);
  if (!self->running) {
    g_mutex_unlock (&self->lock);
    return;
  }
  self->running = FALSE;
  for (GList *l = self->tracks->head; l; l = l->next)
    ((SinkTrack *) l->data)->cancelled = TRUE;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);

  g_thread_join (self->writer);
  self->writer = NULL;
}

#include "player_service.h"

#include "audio/sink.h"

#include <string.h>
#include "native_engine.h"
#include "audio/dsp.h"

struct _SpotifyNativePlayerService {
  GObject parent_instance;
  GTask *task;
  GCancellable *cancellable;
  SpotifyNativeEngineControl *control;
  GMainContext *main_context;
  gchar *track_uri;
  gchar *pending_uri;   /* track requested while another was still playing */
  gchar *retry_uri;     /* a failed track, being given one second chance */
  gboolean unavailable; /* set when the server has no file for this track */
  gint volume_percent;
  gdouble  eq_gains[SPOTIFYGTK_EQ_BANDS];
  gint     output_rate;   /* 0 = follow the stream */
  gboolean eq_enabled;
  guint position_timer_id;   /* polls the engine control for playback position */
  SpotifyNativePlayerState state;

  /*
   * Tracks handed to the sink and not yet finished sounding.
   *
   * There can be two: the one being heard, and the one already decoding behind
   * it. A run finishes when it has pushed its last frame, several seconds
   * before that audio is played, and that overlap is what removes the gap --
   * so "the most recently started run" and "the track the user is hearing" are
   * different things, and the UI has to follow the second.
   */
  GPtrArray *inflight;       /* InflightTrack* */
  guint64    audible_seq;    /* sink slot currently sounding */

  /* A seek that has to wait for a run to exist before it can be applied; see
   * spotifygtk_player_service_seek(). Paired with its URI so a seek meant for
   * one track cannot land on whatever happens to start next. */
  gchar     *pending_seek_uri;
  gint64     pending_seek_ms;
};

typedef struct {
  gchar                      *uri;
  SpotifyNativeEngineControl *control;   /* referenced */
  GCancellable               *cancellable;  /* referenced; the sink watches it */
} InflightTrack;

static void
inflight_track_free (gpointer data)
{
  InflightTrack *t = data;
  g_free (t->uri);
  spotifygtk_native_engine_control_free (t->control);
  g_clear_object (&t->cancellable);
  g_free (t);
}

typedef struct {
  SpotifyNativePlayerService *service;
  SpotifyNativePlayerState    state;
  gchar                      *message;
} ProgressEvent;

G_DEFINE_FINAL_TYPE (SpotifyNativePlayerService, spotifygtk_player_service, G_TYPE_OBJECT)

enum { STATE_CHANGED, POSITION_CHANGED, NOW_PLAYING_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

#define POSITION_POLL_MS 250

/* Poll the engine control for the current position and announce it. Runs on
 * the service's main context while a track is active; stops when the control
 * goes away. */
/*
 * The control of the track being heard, or the one starting if none is.
 *
 * Every transport command has to go through this rather than self->control.
 * A run finishes when it has pushed its last frame, seconds before that audio
 * is played, at which point self->control is cleared and then replaced by the
 * next track's -- so during a handover, pausing self->control either did
 * nothing or paused the decode of a track nobody was listening to yet, while
 * the audible one played on. That is "the play button stopped working".
 *
 * Looked up rather than cached, so a command that arrives between position
 * polls still finds the right track.
 */
static SpotifyNativeEngineControl *
active_control (SpotifyNativePlayerService *self)
{
  guint64 seq = spotifygtk_audio_sink_current_seq (spotifygtk_audio_sink_get ());
  if (seq != 0) {
    for (guint i = 0; i < self->inflight->len; i++) {
      InflightTrack *t = g_ptr_array_index (self->inflight, i);
      if (spotifygtk_native_engine_control_get_sink_seq (t->control) == seq)
        return t->control;
    }
  }
  /* Nothing sounding yet: the track just started is the one a command means. */
  return self->control;
}

/*
 * Drop audio the listener has just decided against.
 *
 * A track's engine run finishes well before its audio does -- up to the
 * sink's thirty seconds of buffer -- and that overlap is exactly what makes
 * the handover gapless. The cost is that for those seconds the sounding track
 * is an older run, and stop() only ever cancelled self->cancellable, which by
 * then belongs to a newer one. Nothing cancelled the run whose audio was
 * actually coming out of the speakers.
 *
 * So picking another song left up to half a minute of the previous one queued
 * ahead of it, and a seek into the tail restarted the track behind that same
 * backlog: the position jumped to where it was asked for, seconds late.
 *
 * Cancelling a track's cancellable is what makes the sink abandon it,
 * buffered frames and the device's own buffer included -- see the writer in
 * sink.c. Only ever called for an explicit action: a natural handover must
 * not come through here, or gapless stops being gapless.
 */
static void
abandon_queued_audio (SpotifyNativePlayerService *self)
{
  for (guint i = 0; i < self->inflight->len; i++) {
    InflightTrack *t = g_ptr_array_index (self->inflight, i);
    if (t->cancellable)
      g_cancellable_cancel (t->cancellable);
  }
}

/*
 * Reconcile what is sounding with what we are holding.
 *
 * Polled rather than pushed from the sink: this runs on the service's main
 * context, where the signal can be emitted straight to the UI, and the sink's
 * writer thread is the last place that should be reaching into widgets. At
 * four times a second the handover is announced well inside a frame.
 *
 * Returns the control of the audible track, or NULL if nothing is.
 */
static SpotifyNativeEngineControl *
sync_audible_track (SpotifyNativePlayerService *self)
{
  SpotifyAudioSink *sink = spotifygtk_audio_sink_get ();
  guint64 seq = spotifygtk_audio_sink_current_seq (sink);

  /* Drop anything the sink has finished with. */
  for (guint i = self->inflight->len; i > 0; i--) {
    InflightTrack *t = g_ptr_array_index (self->inflight, i - 1);
    guint64 t_seq = spotifygtk_native_engine_control_get_sink_seq (t->control);
    if (t_seq != 0 && spotifygtk_audio_sink_track_done (sink, t_seq))
      g_ptr_array_remove_index (self->inflight, i - 1);
  }

  InflightTrack *audible = NULL;
  for (guint i = 0; i < self->inflight->len; i++) {
    InflightTrack *t = g_ptr_array_index (self->inflight, i);
    if (spotifygtk_native_engine_control_get_sink_seq (t->control) == seq) {
      audible = t;
      break;
    }
  }

  if (seq != self->audible_seq && audible) {
    self->audible_seq = seq;
    g_message ("player-service: now sounding %s (sink slot %" G_GUINT64_FORMAT ")",
               audible->uri ? audible->uri : "(resume)", seq);
    g_signal_emit (self, signals[NOW_PLAYING_CHANGED], 0, audible->uri);
  }

  return audible ? audible->control : NULL;
}

static gboolean
poll_position (gpointer user_data)
{
  SpotifyNativePlayerService *self = user_data;

  SpotifyNativeEngineControl *audible = sync_audible_track (self);

  /*
   * Position comes from the track being heard, and from nothing else.
   *
   * Falling back to self->control was wrong in the one case that matters: once
   * the next track starts decoding, self->control is its control, sitting at
   * zero, while the previous track is still playing. Reporting that snapped
   * the progress bar to 0:00 mid-song. If the audible track cannot be
   * identified, the honest answer is to say nothing and leave the last
   * position on screen.
   */
  SpotifyNativeEngineControl *from = audible;
  if (!from) {
    /* Nothing sounding yet. Before the first frame reaches the device the
     * starting track is the right source -- that is how the bar shows 0:00 of
     * the new length while it buffers. */
    if (spotifygtk_audio_sink_current_seq (spotifygtk_audio_sink_get ()) == 0)
      from = self->control;
  }
  if (!from) {
    if (self->inflight->len == 0 && !self->control) {
      self->position_timer_id = 0;
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  gint64 position_ms = spotifygtk_native_engine_control_get_position_ms (from);
  g_signal_emit (self, signals[POSITION_CHANGED], 0, position_ms);
  return G_SOURCE_CONTINUE;
}

static void
start_position_timer (SpotifyNativePlayerService *self)
{
  if (self->position_timer_id != 0)
    return;
  self->position_timer_id = g_timeout_add (POSITION_POLL_MS, poll_position, self);
}

static void
stop_position_timer (SpotifyNativePlayerService *self)
{
  g_clear_handle_id (&self->position_timer_id, g_source_remove);
}

static void
emit_state (SpotifyNativePlayerService *self, SpotifyNativePlayerState state,
            const gchar *message)
{
  self->state = state;
  g_message ("player-service: %s", message);
  g_signal_emit (self, signals[STATE_CHANGED], 0, state, message);
}

static gboolean
dispatch_progress (gpointer user_data)
{
  ProgressEvent *event = user_data;
  emit_state (event->service, event->state, event->message);
  g_object_unref (event->service);
  g_free (event->message);
  g_free (event);
  return G_SOURCE_REMOVE;
}

static void
engine_progress (SpotifyNativeEngineStage stage, const gchar *message,
                 gpointer user_data)
{
  SpotifyNativePlayerService *self = user_data;
  ProgressEvent *event = g_new0 (ProgressEvent, 1);
  event->service = g_object_ref (self);
  event->message = g_strdup (message);
  switch (stage) {
    case SPOTIFYGTK_ENGINE_CONNECTING:
      event->state = SPOTIFYGTK_PLAYER_CONNECTING;
      break;
    case SPOTIFYGTK_ENGINE_BUFFERING:
      event->state = SPOTIFYGTK_PLAYER_BUFFERING;
      break;
    case SPOTIFYGTK_ENGINE_PLAYING:
      event->state = SPOTIFYGTK_PLAYER_PLAYING;
      break;
    case SPOTIFYGTK_ENGINE_UNAVAILABLE:
      /* Noted for the finish handler, which decides between retrying and
       * moving on. The state itself stays IDLE: this is not an error in the
       * player, and painting it as one flashes a broken-looking bar before
       * the next track starts. */
      self->unavailable = TRUE;
      event->state = SPOTIFYGTK_PLAYER_IDLE;
      break;
    case SPOTIFYGTK_ENGINE_IDLE:
    default:
      event->state = SPOTIFYGTK_PLAYER_IDLE;
      break;
  }
  g_main_context_invoke (self->main_context, dispatch_progress, event);
}

static void
run_engine_thread (GTask *task, gpointer source_object, gpointer task_data,
                   GCancellable *cancellable)
{
  SpotifyNativePlayerService *self = source_object;
  gboolean ok = spotifygtk_native_engine_run (cancellable, engine_progress,
                                               source_object, self->control,
                                               self->track_uri);
  if (g_cancellable_is_cancelled (cancellable))
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Playback stopped");
  else if (ok)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Native playback failed");
  (void) source_object; (void) task_data;
}

static void
on_engine_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativePlayerService *self = user_data;
  g_autoptr(GError) error = NULL;
  gboolean ok = g_task_propagate_boolean (G_TASK (result), &error);

  g_clear_object (&self->task);
  g_clear_pointer (&self->control, spotifygtk_native_engine_control_free);
  g_clear_object (&self->cancellable);

  /* A track queued by start_uri() while this one was still running. Start it
   * before reporting IDLE/ERROR, so the UI never flashes "stopped" for a
   * switch the user experiences as one continuous action. */
  if (self->pending_uri) {
    g_autofree gchar *next = g_steal_pointer (&self->pending_uri);
    g_autoptr(GError) start_err = NULL;

    if (spotifygtk_player_service_start_uri (self, next, &start_err)) {
      g_object_unref (self);
      (void) source;
      return;
    }

    g_warning ("player-service: could not start queued track: %s",
               start_err ? start_err->message : "unknown error");
  }

  /*
   * One automatic retry for a failed track.
   *
   * The common failure is an audio key that never comes back, which is what a
   * dead AP link looks like from here: the link is dropped on that timeout so
   * the *next* attempt reconnects, but the attempt that discovered it is lost.
   * From the outside that is a song that simply does not play, at random,
   * with the next one fine -- so the attempt that discovered the problem is
   * the one worth repeating.
   *
   * Once only, and tracked by URI, so a track that genuinely cannot play
   * fails twice and stops rather than looping.
   */
  /*
   * A track the server has no file for cannot be retried into existence, and
   * the retry below is specifically for a dropped AP link. Retrying doubled
   * the dead air before playback simply stopped.
   */
  if (!ok && self->unavailable) {
    g_message ("player-service: %s is unavailable; not retrying",
               self->track_uri ? self->track_uri : "(no uri)");
    self->unavailable = FALSE;
    emit_state (self, SPOTIFYGTK_PLAYER_UNAVAILABLE, "This track is not available.");
    g_object_unref (self);
    (void) source;
    return;
  }

  if (!ok && self->track_uri && g_strcmp0 (self->retry_uri, self->track_uri) != 0) {
    g_autofree gchar *again = g_strdup (self->track_uri);
    g_autoptr(GError) retry_err = NULL;

    g_free (self->retry_uri);
    self->retry_uri = g_strdup (again);

    g_message ("player-service: %s failed; retrying once (a dropped AP link "
               "fails the attempt that discovers it)", again);

    if (spotifygtk_player_service_start_uri (self, again, &retry_err)) {
      g_object_unref (self);
      (void) source;
      return;
    }
    g_warning ("player-service: retry could not start: %s",
               retry_err ? retry_err->message : "unknown error");
  }

  /* Cleared on success so the next failure of this track gets its own retry. */
  if (ok)
    g_clear_pointer (&self->retry_uri, g_free);

  /*
   * Do not stop the position timer here. This run is finished producing, but
   * the sink may still have several seconds of its audio to play, and the
   * timer is what announces the handover and keeps position moving until it
   * really is done. poll_position retires itself once nothing is in flight.
   */
  emit_state (self, ok ? SPOTIFYGTK_PLAYER_IDLE : SPOTIFYGTK_PLAYER_ERROR,
              ok ? "Playback completed." : error->message);
  g_object_unref (self);
  (void) source;
}

gboolean
spotifygtk_player_service_start (SpotifyNativePlayerService *self, GError **error)
{
  return spotifygtk_player_service_start_uri (self, NULL, error);
}

gboolean
spotifygtk_player_service_start_uri (SpotifyNativePlayerService *self,
                                     const gchar *track_uri, GError **error)
{
  self->unavailable = FALSE;
  g_return_val_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self), FALSE);

  /* Picking a different track while one is playing used to fail outright
   * with "playback is already active", so the first song kept playing and
   * clicking anything else did nothing. The engine runs one track per
   * worker, so switching means stopping the current one and starting the
   * new one once it has actually finished unwinding -- remember it here and
   * let on_engine_finished pick it up. */
  if (self->task) {
    g_free (self->pending_uri);
    self->pending_uri = g_strdup (track_uri);

    /* Everything already decoded belongs to a track the listener has moved
     * on from. Without this the new one waits behind it. */
    abandon_queued_audio (self);

    /* Resume first: a paused worker is blocked on the pause condition and
     * would never observe the cancellation. */
    spotifygtk_native_engine_control_resume (self->control);
    spotifygtk_player_service_stop (self);
    return TRUE;
  }

  g_free (self->track_uri);
  self->track_uri = g_strdup (track_uri);
  self->cancellable = g_cancellable_new ();
  self->control = spotifygtk_native_engine_control_new ();

  /* A seek that was waiting for this run. Requested before the engine starts,
   * so it is consumed at the first wait point rather than after a second of
   * the track has already been heard from the top. */
  if (self->pending_seek_uri && g_strcmp0 (self->pending_seek_uri, track_uri) == 0) {
    spotifygtk_native_engine_control_request_seek (self->control, self->pending_seek_ms);
    g_clear_pointer (&self->pending_seek_uri, g_free);
    self->pending_seek_ms = 0;
  } else if (self->pending_seek_uri) {
    /* Something else started first; the seek is no longer meaningful. */
    g_clear_pointer (&self->pending_seek_uri, g_free);
    self->pending_seek_ms = 0;
  }
  spotifygtk_native_engine_control_set_volume (self->control,
                                              self->volume_percent / 100.0);
  spotifygtk_native_engine_control_set_eq (self->control,
                                           self->eq_gains, self->eq_enabled);
  spotifygtk_native_engine_control_set_output_rate (self->control, self->output_rate);
  if (!self->main_context)
    self->main_context = g_main_context_ref_thread_default ();
  if (!self->main_context)
    self->main_context = g_main_context_ref (g_main_context_default ());
  InflightTrack *entry = g_new0 (InflightTrack, 1);
  entry->uri     = g_strdup (track_uri);
  entry->control = spotifygtk_native_engine_control_ref (self->control);
  entry->cancellable = self->cancellable ? g_object_ref (self->cancellable) : NULL;
  g_ptr_array_add (self->inflight, entry);

  self->task = g_task_new (self, self->cancellable, on_engine_finished, g_object_ref (self));
  /* Wait for the worker to finish before emitting completion. The current
   * engine APIs do not yet accept a cancellable at every network hop; an
   * early callback would risk disposing state still used by those hops. */
  g_task_set_return_on_cancel (self->task, FALSE);
  emit_state (self, SPOTIFYGTK_PLAYER_CONNECTING, "Native playback engine is starting.");
  g_task_run_in_thread (self->task, run_engine_thread);
  start_position_timer (self);
  (void) error;
  return TRUE;
}

void
spotifygtk_player_service_drop_queued_audio (SpotifyNativePlayerService *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));
  abandon_queued_audio (self);
}

void
spotifygtk_player_service_stop (SpotifyNativePlayerService *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));
  if (self->task) {
    emit_state (self, SPOTIFYGTK_PLAYER_STOPPING,
                "Stop requested; finishing the current engine operation…");
    spotifygtk_native_engine_control_resume (self->control);
    if (self->cancellable)
      g_cancellable_cancel (self->cancellable);
  }
}

void
spotifygtk_player_service_pause (SpotifyNativePlayerService *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));
  SpotifyNativeEngineControl *c = active_control (self);
  if (!c)
    return;
  /* Must report PAUSED, not PLAYING. Emitting PLAYING here left the
   * playback bar showing a pause icon while actually paused, so the next
   * click emitted "pause" again instead of "play" -- resume was
   * unreachable and the track appeared stuck. */
  spotifygtk_native_engine_control_pause (c);
  emit_state (self, SPOTIFYGTK_PLAYER_PAUSED, "Playback paused; buffered audio is retained.");
}

void
spotifygtk_player_service_resume (SpotifyNativePlayerService *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));
  SpotifyNativeEngineControl *c = active_control (self);
  if (!c)
    return;
  spotifygtk_native_engine_control_resume (c);
  emit_state (self, SPOTIFYGTK_PLAYER_PLAYING, "Playback resumed.");
}

gboolean
spotifygtk_player_service_is_paused (SpotifyNativePlayerService *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self), FALSE);
  SpotifyNativeEngineControl *c = active_control (self);
  return c && spotifygtk_native_engine_control_is_paused (c);
}

void
spotifygtk_player_service_seek (SpotifyNativePlayerService *self, gint64 position_ms)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));

  SpotifyNativeEngineControl *c = active_control (self);
  if (!c)
    return;

  /*
   * Seeking into a track whose producer has already exited.
   *
   * A track's engine run finishes several seconds before its audio does --
   * that overlap is what makes the handover gapless -- so for those last
   * seconds the sounding track has no engine loop left to consume a seek.
   * request_seek() would set a flag nobody reads: the drag appeared to do
   * nothing and the slider snapped back to wherever the sink had got to,
   * which is what "it is impossible to bring it to the start, it auto
   * reverts" was.
   *
   * The run is gone, so there is nothing to seek; the track has to be played
   * again from the target instead. The URI is known because the sounding
   * track is in `inflight`, which is what active_control() just matched on.
   */
  if (c != self->control) {
    const gchar *uri = NULL;
    guint64 seq = spotifygtk_audio_sink_current_seq (spotifygtk_audio_sink_get ());
    for (guint i = 0; i < self->inflight->len; i++) {
      InflightTrack *t = g_ptr_array_index (self->inflight, i);
      if (spotifygtk_native_engine_control_get_sink_seq (t->control) == seq) {
        uri = t->uri;
        break;
      }
    }

    if (uri) {
      g_autofree gchar *want = g_strdup (uri);
      g_autoptr(GError) err = NULL;
      g_message ("player-service: seek into a track whose run has ended; "
                 "restarting it at %" G_GINT64_FORMAT " ms", position_ms);
      g_free (self->pending_seek_uri);
      self->pending_seek_uri = g_strdup (want);
      self->pending_seek_ms  = position_ms;

      /* The tail still queued is audio from before the seek. Kept, it plays
       * on while the restarted track waits behind it, which is the drag
       * appearing to take effect several seconds late. */
      abandon_queued_audio (self);
      if (!spotifygtk_player_service_start_uri (self, want, &err)) {
        g_warning ("player-service: could not restart for the seek: %s",
                   err ? err->message : "unknown");
        g_clear_pointer (&self->pending_seek_uri, g_free);
        self->pending_seek_ms = 0;
      }
      return;
    }
  }

  /* The engine consumes this at its next wait point and preserves pause state
   * across the seek (see do_seek in the engine). */
  spotifygtk_native_engine_control_request_seek (c, position_ms);
}

gboolean
spotifygtk_player_service_is_active (SpotifyNativePlayerService *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self), FALSE);
  return self->task != NULL;
}

SpotifyNativePlayerState
spotifygtk_player_service_get_state (SpotifyNativePlayerService *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self), SPOTIFYGTK_PLAYER_ERROR);
  return self->state;
}

static void
spotifygtk_player_service_dispose (GObject *object)
{
  SpotifyNativePlayerService *self = SPOTIFYGTK_PLAYER_SERVICE (object);
  stop_position_timer (self);
  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  spotifygtk_native_engine_control_resume (self->control);
  g_clear_pointer (&self->control, spotifygtk_native_engine_control_free);
  g_clear_object (&self->task);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->track_uri, g_free);
  g_clear_pointer (&self->pending_uri, g_free);
  g_clear_pointer (&self->retry_uri, g_free);
  g_clear_pointer (&self->pending_seek_uri, g_free);
  g_clear_pointer (&self->inflight, g_ptr_array_unref);
  g_clear_pointer (&self->main_context, g_main_context_unref);
  G_OBJECT_CLASS (spotifygtk_player_service_parent_class)->dispose (object);
}

static void
spotifygtk_player_service_class_init (SpotifyNativePlayerServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_player_service_dispose;
  signals[STATE_CHANGED] = g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);
  signals[POSITION_CHANGED] = g_signal_new ("position-changed", G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 1, G_TYPE_INT64);
  /* The track actually being heard changed. Emitted at the handover, which is
   * later than the moment its decode started -- see sync_audible_track(). */
  signals[NOW_PLAYING_CHANGED] = g_signal_new ("now-playing-changed",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                               G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void spotifygtk_player_service_init (SpotifyNativePlayerService *self)
{
  self->inflight = g_ptr_array_new_with_free_func (inflight_track_free);
  self->state = SPOTIFYGTK_PLAYER_IDLE;
  self->volume_percent = 100;
}

void
spotifygtk_player_service_set_volume (SpotifyNativePlayerService *self, gint percent)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));

  self->volume_percent = CLAMP (percent, 0, 100);

  /* Applies immediately when something is playing; otherwise it is picked
   * up by the next control created in start_uri(). */
  /* Every track still in flight, not just the producing one: during a
   * handover the audible track is a different control, and a volume change
   * that skipped it would not be heard until the next song. */
  for (guint i = 0; self->inflight && i < self->inflight->len; i++)
    spotifygtk_native_engine_control_set_volume (
      ((InflightTrack *) g_ptr_array_index (self->inflight, i))->control,
      self->volume_percent / 100.0);
  if (self->control)
    spotifygtk_native_engine_control_set_volume (self->control,
                                                 self->volume_percent / 100.0);
}

void
spotifygtk_player_service_set_eq (SpotifyNativePlayerService *self,
                                  const gdouble *gains_db, gboolean enabled)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));

  if (gains_db)
    memcpy (self->eq_gains, gains_db, sizeof self->eq_gains);
  self->eq_enabled = enabled;

  for (guint i = 0; self->inflight && i < self->inflight->len; i++)
    spotifygtk_native_engine_control_set_eq (
      ((InflightTrack *) g_ptr_array_index (self->inflight, i))->control,
      self->eq_gains, enabled);
  if (self->control)
    spotifygtk_native_engine_control_set_eq (self->control, self->eq_gains, enabled);
}

gint
spotifygtk_player_service_get_volume (SpotifyNativePlayerService *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self), 100);
  return self->volume_percent;
}

SpotifyNativePlayerService *
spotifygtk_player_service_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_PLAYER_SERVICE, NULL);
}

void
spotifygtk_player_service_set_output_rate (SpotifyNativePlayerService *self, gint hz)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));

  self->output_rate = hz > 0 ? hz : 0;

  /* Takes effect on the next track: the device is already open at the old rate
   * and reopening it mid-stream would gap the audio. */
  if (self->control)
    spotifygtk_native_engine_control_set_output_rate (self->control, self->output_rate);
}

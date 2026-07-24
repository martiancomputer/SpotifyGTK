#include "player_service.h"

#include <string.h>
#include "native_engine.h"

struct _SpotifyNativePlayerService {
  GObject parent_instance;
  GTask *task;
  GCancellable *cancellable;
  SpotifyNativeEngineControl *control;
  GMainContext *main_context;
  gchar *track_uri;
  gchar *pending_uri;   /* track requested while another was still playing */
  gint volume_percent;
  gdouble  eq_gains[10];
  gboolean eq_enabled;
  guint position_timer_id;   /* polls the engine control for playback position */
  SpotifyNativePlayerState state;
};

typedef struct {
  SpotifyNativePlayerService *service;
  SpotifyNativePlayerState    state;
  gchar                      *message;
} ProgressEvent;

G_DEFINE_FINAL_TYPE (SpotifyNativePlayerService, spotifygtk_player_service, G_TYPE_OBJECT)

enum { STATE_CHANGED, POSITION_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

#define POSITION_POLL_MS 250

/* Poll the engine control for the current position and announce it. Runs on
 * the service's main context while a track is active; stops when the control
 * goes away. */
static gboolean
poll_position (gpointer user_data)
{
  SpotifyNativePlayerService *self = user_data;
  if (!self->control) {
    self->position_timer_id = 0;
    return G_SOURCE_REMOVE;
  }
  gint64 position_ms = spotifygtk_native_engine_control_get_position_ms (self->control);
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

  /* Playback truly ended (nothing pending): stop polling position. */
  stop_position_timer (self);
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
  spotifygtk_native_engine_control_set_volume (self->control,
                                              self->volume_percent / 100.0);
  spotifygtk_native_engine_control_set_eq (self->control,
                                           self->eq_gains, self->eq_enabled);
  if (!self->main_context)
    self->main_context = g_main_context_ref_thread_default ();
  if (!self->main_context)
    self->main_context = g_main_context_ref (g_main_context_default ());
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
  if (!self->task || !self->control)
    return;
  /* Must report PAUSED, not PLAYING. Emitting PLAYING here left the
   * playback bar showing a pause icon while actually paused, so the next
   * click emitted "pause" again instead of "play" -- resume was
   * unreachable and the track appeared stuck. */
  spotifygtk_native_engine_control_pause (self->control);
  emit_state (self, SPOTIFYGTK_PLAYER_PAUSED, "Playback paused; buffered audio is retained.");
}

void
spotifygtk_player_service_resume (SpotifyNativePlayerService *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));
  if (!self->task || !self->control)
    return;
  spotifygtk_native_engine_control_resume (self->control);
  emit_state (self, SPOTIFYGTK_PLAYER_PLAYING, "Playback resumed.");
}

gboolean
spotifygtk_player_service_is_paused (SpotifyNativePlayerService *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self), FALSE);
  return self->control && spotifygtk_native_engine_control_is_paused (self->control);
}

void
spotifygtk_player_service_seek (SpotifyNativePlayerService *self, gint64 position_ms)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYER_SERVICE (self));
  if (!self->task || !self->control)
    return;
  /* The engine consumes this at its next wait point and preserves pause state
   * across the seek (see do_seek in the engine). */
  spotifygtk_native_engine_control_request_seek (self->control, position_ms);
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
}

static void spotifygtk_player_service_init (SpotifyNativePlayerService *self)
{
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

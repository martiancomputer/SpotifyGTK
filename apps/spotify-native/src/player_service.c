#include "player_service.h"
#include "native_engine.h"

struct _SpotifyNativePlayerService {
  GObject parent_instance;
  GTask *task;
  GCancellable *cancellable;
  SpotifyNativeEngineControl *control;
  GMainContext *main_context;
  gchar *track_uri;
  SpotifyNativePlayerState state;
};

typedef struct {
  SpotifyNativePlayerService *service;
  SpotifyNativePlayerState    state;
  gchar                      *message;
} ProgressEvent;

G_DEFINE_FINAL_TYPE (SpotifyNativePlayerService, spotifygtk_player_service, G_TYPE_OBJECT)

enum { STATE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

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
  emit_state (self, ok ? SPOTIFYGTK_PLAYER_IDLE : SPOTIFYGTK_PLAYER_ERROR,
              ok ? "Playback completed." : error->message);
  g_clear_object (&self->task);
  g_clear_pointer (&self->control, spotifygtk_native_engine_control_free);
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
  if (self->task) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY, "Playback is already active");
    return FALSE;
  }
  g_free (self->track_uri);
  self->track_uri = g_strdup (track_uri);
  self->cancellable = g_cancellable_new ();
  self->control = spotifygtk_native_engine_control_new ();
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
  spotifygtk_native_engine_control_pause (self->control);
  emit_state (self, SPOTIFYGTK_PLAYER_PLAYING, "Playback paused; buffered audio is retained.");
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
  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  spotifygtk_native_engine_control_resume (self->control);
  g_clear_pointer (&self->control, spotifygtk_native_engine_control_free);
  g_clear_object (&self->task);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->track_uri, g_free);
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
}

static void spotifygtk_player_service_init (SpotifyNativePlayerService *self)
{
  self->state = SPOTIFYGTK_PLAYER_IDLE;
}

SpotifyNativePlayerService *
spotifygtk_player_service_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_PLAYER_SERVICE, NULL);
}

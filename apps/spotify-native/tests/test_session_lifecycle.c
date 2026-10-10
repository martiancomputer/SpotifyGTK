/* Exercise the real session worker without OAuth, sockets, or GTK. Only the
 * credential store and AP handshake are replaced; logout, cancellation,
 * generations, login5 result handling and READY dispatch are production code. */
#include "spotify/native_auth.h"
#include "spotify/ap.h"

static gint credential_revision;
static gint connect_calls;

static NativeAuth *fixture_auth_new (void);
static gboolean fixture_has_credentials (NativeAuth *auth);
static const gchar *fixture_get_token (NativeAuth *auth);
static void fixture_connect (SpotifyApSession *ap, const gchar *token,
                             GAsyncReadyCallback callback, gpointer data);

#define native_auth_new fixture_auth_new
#define native_auth_has_credentials fixture_has_credentials
#define native_auth_has_valid_token fixture_has_credentials
#define native_auth_get_token fixture_get_token
#define spotifygtk_ap_session_connect fixture_connect
#include "../src/spotify/session.c"
#undef native_auth_new
#undef native_auth_has_credentials
#undef native_auth_has_valid_token
#undef native_auth_get_token
#undef spotifygtk_ap_session_connect

static NativeAuth *
fixture_auth_new (void)
{
  /* No real credential file is opened, even on a developer's machine. */
  return (NativeAuth *) g_object_new (G_TYPE_OBJECT, NULL);
}

static gboolean
fixture_has_credentials (NativeAuth *auth)
{
  (void) auth;
  return TRUE;
}

static const gchar *
fixture_get_token (NativeAuth *auth)
{
  (void) auth;
  return g_atomic_int_get (&credential_revision) == 0
    ? "fixture-first-credential" : "fixture-replacement-credential";
}

static void
fixture_connect (SpotifyApSession *ap, const gchar *token,
                 GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr(SpotifyNativeSession) self = session_auth_step_take (data);
  g_assert_nonnull (self);
  g_assert_true (g_main_context_is_owner (self->context));
  g_assert_false (g_cancellable_is_cancelled (self->cancellable));
  g_assert_false (g_atomic_int_get (&self->signed_out));
  g_assert_cmpstr (self->login_token, ==, fixture_get_token (NULL));

  /* Supply a synthetic login5 completion on the same worker context, without
   * contacting any authentication service or Spotify Connect endpoint. */
  on_login5_result (g_atomic_int_get (&credential_revision) == 0
                     ? "fixture-first-bearer" : "fixture-replacement-bearer",
                   3600, NULL, session_auth_step_new (self));
  g_atomic_int_inc (&connect_calls);
  (void) ap; (void) token; (void) callback;
}

typedef struct {
  GMainContext *caller;
  SpotifyNativeSession *session;
  guint ready_events;
  guint idle_events;
} Fixture;

static void
on_state (SpotifyNativeSession *session, gint state,
          const gchar *message, gpointer data)
{
  Fixture *f = data;
  if (state == SPOTIFYGTK_SESSION_READY)
    f->ready_events++;
  if (state == SPOTIFYGTK_SESSION_IDLE)
    f->idle_events++;
  (void) session; (void) message;
}

static void
setup (Fixture *f, gconstpointer data)
{
  g_atomic_int_set (&credential_revision, 0);
  g_atomic_int_set (&connect_calls, 0);
  f->caller = g_main_context_new ();
  g_main_context_push_thread_default (f->caller);
  f->session = spotifygtk_native_session_new ();
  g_signal_connect (f->session, "state-changed", G_CALLBACK (on_state), f);
  (void) data;
}

static void
wait_for_events (Fixture *f, guint ready, guint idle)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (f->ready_events < ready || f->idle_events < idle) {
    g_assert_cmpint (g_get_monotonic_time (), <, deadline);
    while (g_main_context_iteration (f->caller, FALSE)) {}
    g_usleep (1000);
  }
}

static void
teardown (Fixture *f, gconstpointer data)
{
  spotifygtk_native_session_disconnect (f->session);
  wait_for_events (f, f->ready_events, f->idle_events + 1);
  while (g_main_context_iteration (f->caller, FALSE)) {}
  g_object_unref (f->session);
  g_main_context_pop_thread_default (f->caller);
  g_main_context_unref (f->caller);
  (void) data;
}

static void
test_sign_out_then_start (Fixture *f, gconstpointer data)
{
  spotifygtk_native_session_start (f->session);
  wait_for_events (f, 1, 0);
  GThread *original_worker = f->session->thread;

  for (guint i = 1; i <= 3; i++) {
    spotifygtk_native_session_disconnect (f->session);
    wait_for_events (f, i, i);
    g_assert_cmpint (spotifygtk_native_session_get_state (f->session), ==,
                     SPOTIFYGTK_SESSION_IDLE);

    g_atomic_int_set (&credential_revision, 1);
    spotifygtk_native_session_start (f->session);
    spotifygtk_native_session_start (f->session); /* connecting is idempotent */
    wait_for_events (f, i + 1, i);
    g_assert_true (f->session->thread == original_worker);
    g_assert_cmpstr (f->session->bearer_token, ==, "fixture-replacement-bearer");
    g_assert_cmpint (g_atomic_int_get (&connect_calls), ==, i + 1);
  }
  (void) data;
}

static void
test_ready_start_is_noop (Fixture *f, gconstpointer data)
{
  spotifygtk_native_session_start (f->session);
  wait_for_events (f, 1, 0);
  gint generation = g_atomic_int_get (&f->session->auth_generation);
  spotifygtk_native_session_start (f->session);
  g_assert_cmpint (g_atomic_int_get (&f->session->auth_generation), ==, generation);
  g_assert_cmpint (spotifygtk_native_session_get_state (f->session), ==,
                   SPOTIFYGTK_SESSION_READY);
  (void) data;
}

static void
test_sign_in_before_logout_finishes (Fixture *f, gconstpointer data)
{
  spotifygtk_native_session_start (f->session);
  wait_for_events (f, 1, 0);
  SessionAuthStep *old_completion = session_auth_step_new (f->session);
  GThread *original_worker = f->session->thread;

  /* No main-loop turn between logout and the new browser approval. The old
   * state can still read READY, but signed_out must trigger a fresh login. */
  spotifygtk_native_session_disconnect (f->session);
  g_atomic_int_set (&credential_revision, 1);
  spotifygtk_native_session_start (f->session);
  wait_for_events (f, 2, 0);
  g_assert_true (f->session->thread == original_worker);
  g_assert_cmpint (g_atomic_int_get (&connect_calls), ==, 2);

  /* A delayed result from before logout cannot replace the new bearer. */
  on_login5_result ("fixture-stale-bearer", 3600, NULL, old_completion);
  g_assert_cmpstr (f->session->bearer_token, ==, "fixture-replacement-bearer");
  (void) data;
}

int
main (int argc, char **argv)
{
  g_setenv ("SPOTIFY_DISABLE_CONNECT", "1", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/session/logout-login-reuses-worker", Fixture, NULL,
              setup, test_sign_out_then_start, teardown);
  g_test_add ("/session/ready-start-is-noop", Fixture, NULL,
              setup, test_ready_start_is_noop, teardown);
  g_test_add ("/session/login-before-logout-finishes", Fixture, NULL,
              setup, test_sign_in_before_logout_finishes, teardown);
  return g_test_run ();
}

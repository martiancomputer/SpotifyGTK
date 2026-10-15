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

static void
test_connect_payload_decode (void)
{
  const gchar *json = "{\"command\":{\"endpoint\":\"pause\"}}";
  g_autofree gchar *plain = g_base64_encode ((const guint8 *) json, strlen (json));
  g_autoptr(GBytes) decoded = connect_decode_payload (plain, FALSE);
  g_assert_cmpmem (g_bytes_get_data (decoded, NULL), g_bytes_get_size (decoded),
                   json, strlen (json));
  g_autoptr(GZlibCompressor) compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, 6);
  g_autoptr(GOutputStream) sink = g_memory_output_stream_new_resizable ();
  g_autoptr(GOutputStream) encoded = g_converter_output_stream_new (sink, G_CONVERTER (compressor));
  g_assert_true (g_output_stream_write_all (encoded, json, strlen (json), NULL, NULL, NULL));
  g_assert_true (g_output_stream_close (encoded, NULL, NULL));
  g_autofree gchar *gzip = g_base64_encode (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (sink)),
    g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (sink)));
  g_clear_pointer (&decoded, g_bytes_unref);
  decoded = connect_decode_payload (gzip, TRUE);
  g_assert_nonnull (decoded);
  g_assert_cmpmem (g_bytes_get_data (decoded, NULL), g_bytes_get_size (decoded), json, strlen (json));
  g_assert_null (connect_decode_payload (plain, TRUE));
  g_assert_null (connect_decode_payload ("", FALSE));

  /* A small gzip packet must not expand without bound. */
  g_autofree gchar *large = g_malloc0 (4 * 1024 * 1024 + 1);
  g_autoptr(GOutputStream) large_sink = g_memory_output_stream_new_resizable ();
  g_autoptr(GZlibCompressor) large_compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, 6);
  g_autoptr(GOutputStream) large_encoded = g_converter_output_stream_new (large_sink, G_CONVERTER (large_compressor));
  g_assert_true (g_output_stream_write_all (large_encoded, large, 4 * 1024 * 1024 + 1, NULL, NULL, NULL));
  g_assert_true (g_output_stream_close (large_encoded, NULL, NULL));
  g_autofree gchar *bomb = g_base64_encode (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (large_sink)),
    g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (large_sink)));
  g_assert_null (connect_decode_payload (bomb, TRUE));
}

static void
test_connect_devices (void)
{
  g_autoptr(SpotifyNativeSession) session = spotifygtk_native_session_new ();
  g_autoptr(GByteArray) cluster = g_byte_array_new ();
  for (guint i = 0; i < 4; i++) {
    const gchar *ids[] = { "fixture-computer", "fixture-phone", "fixture-offline", "fixture-hidden" };
    g_autoptr(GByteArray) info = g_byte_array_new ();
    g_autoptr(GByteArray) caps = g_byte_array_new ();
    g_autoptr(GByteArray) entry = g_byte_array_new ();
    pb_write_varint_field (info, 1, 1);
    pb_write_bytes_field (info, 3, (const guint8 *) "Test device", 11);
    pb_write_varint_field (info, 7, i == 1 ? 3 : 1);
    pb_write_varint_field (info, 21, i == 2);
    pb_write_varint_field (caps, 12, i == 3);
    pb_write_message_field (info, 4, caps->data, caps->len);
    pb_write_bytes_field (entry, 1, (const guint8 *) ids[i], strlen (ids[i]));
    pb_write_message_field (entry, 2, info->data, info->len);
    pb_write_message_field (cluster, 4, entry->data, entry->len);
  }
  connect_update_devices (session, cluster->data, cluster->len, "fixture-phone");
  g_autoptr(GPtrArray) snapshot = spotifygtk_native_session_dup_connect_devices (session);
  g_assert_cmpuint (snapshot->len, ==, 3); /* local + two available peers */
  SpotifyNativeConnectDevice *local = g_ptr_array_index (snapshot, 0);
  g_assert_true (local->local);
  g_assert_cmpstr (local->name, ==, "SpotifyGTK");
  SpotifyNativeConnectDevice *phone = g_ptr_array_index (snapshot, 2);
  g_assert_cmpstr (phone->type, ==, "smartphone");
  g_assert_true (phone->active);
  /* A partial update preserves peers, and snapshots don't alias live state. */
  connect_update_devices (session, NULL, 0, session->device_id);
  g_autoptr(GPtrArray) next = spotifygtk_native_session_dup_connect_devices (session);
  g_assert_cmpuint (next->len, ==, 3);
  g_assert_true (((SpotifyNativeConnectDevice *) g_ptr_array_index (next, 0))->active);
  g_assert_true (phone->active);
  dealer_shutdown (session);
  g_assert_null (spotifygtk_native_session_dup_connect_devices (session));
}

static void
test_connect_play_command (void)
{
  g_autoptr(SpotifyNativeSession) session = spotifygtk_native_session_new ();
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser,
    "{\"context\":{\"uri\":\"spotify:playlist:fixture\"},\"options\":{"
    "\"skip_to\":{\"track_uri\":\"spotify:track:fixture\",\"track_index\":7},"
    "\"seek_to\":42000,\"initially_paused\":true,"
    "\"player_options_override\":{\"shuffling_context\":true}}}", -1, NULL));
  ConnectPlayRequest *request = connect_parse_play (session,
    json_node_get_object (json_parser_get_root (parser)));
  g_assert_nonnull (request);
  g_assert_cmpstr (request->track_uri, ==, "spotify:track:fixture");
  g_assert_cmpstr (request->context_uri, ==, "spotify:playlist:fixture");
  g_assert_cmpuint (request->index, ==, 7);
  g_assert_cmpint (request->position, ==, 42000);
  g_assert_true (request->paused);
  g_assert_cmpuint (request->shuffle_mode, ==, 1);
  connect_play_request_free (request);
  g_assert_true (json_parser_load_from_data (parser, "{\"context\":{\"uri\":\"local:track:fixture\"}}", -1, NULL));
  g_assert_null (connect_parse_play (session, json_node_get_object (json_parser_get_root (parser))));
}

int
main (int argc, char **argv)
{
  g_setenv ("SPOTIFY_DISABLE_CONNECT", "1", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/connect/payload-decode", test_connect_payload_decode);
  g_test_add_func ("/connect/device-roster", test_connect_devices);
  g_test_add_func ("/connect/play-command", test_connect_play_command);
  g_test_add ("/session/logout-login-reuses-worker", Fixture, NULL,
              setup, test_sign_out_then_start, teardown);
  g_test_add ("/session/ready-start-is-noop", Fixture, NULL,
              setup, test_ready_start_is_noop, teardown);
  g_test_add ("/session/login-before-logout-finishes", Fixture, NULL,
              setup, test_sign_in_before_logout_finishes, teardown);
  return g_test_run ();
}

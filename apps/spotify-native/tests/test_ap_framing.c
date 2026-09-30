/* Exercise the real receive loop without connecting to Spotify. */
#include "../src/spotify/ap.c"

typedef struct {
  SpotifyApSession *session;
  GSocketConnection *peer;
  guint packets, disconnects;
} Fixture;

static void
received (SpotifyApSession *session, ApCommandId cmd,
          const guint8 *payload, gsize length, gpointer data)
{
  Fixture *f = data;
  g_assert_cmpuint (length, ==, 4);
  g_assert_cmpmem (payload, length, "test", 4);
  f->packets++;
  (void) session;
  (void) cmd;
}

static void
disconnected (SpotifyApSession *session, GError *error, gpointer data)
{
  ((Fixture *) data)->disconnects++;
  g_assert_nonnull (error);
  (void) session;
}

static void
setup (Fixture *f, gconstpointer data)
{
  g_autoptr(GSocketListener) listener = g_socket_listener_new ();
  g_autoptr(GSocketClient) client = g_socket_client_new ();
  g_autoptr(GError) error = NULL;
  guint16 port = g_socket_listener_add_any_inet_port (listener, NULL, &error);
  g_assert_no_error (error);
  f->peer = g_socket_client_connect_to_host (client, "localhost", port, NULL, &error);
  g_assert_no_error (error);
  f->session = spotifygtk_ap_session_new ();
  f->session->connection = g_socket_listener_accept (listener, NULL, NULL, &error);
  g_assert_no_error (error);
  f->session->connected = TRUE;
  guint8 key[32] = {0};
  shannon_key_setup (&f->session->recv_cipher, key, sizeof key);
  spotifygtk_ap_session_set_handler (f->session, AP_CMD_MERCURY_REQ, received, f);
  g_signal_connect (f->session, "disconnected", G_CALLBACK (disconnected), f);
  spotifygtk_ap_session_start_receiving (f->session);
  (void) data;
}

static void
pump (guint milliseconds)
{
  gint64 deadline = g_get_monotonic_time () + milliseconds * 1000;
  do {
    while (g_main_context_iteration (NULL, FALSE)) {}
    g_usleep (1000);
  } while (g_get_monotonic_time () < deadline);
}

static void
teardown (Fixture *f, gconstpointer data)
{
  spotifygtk_ap_session_disconnect (f->session);
  g_io_stream_close (G_IO_STREAM (f->peer), NULL, NULL);
  pump (20); /* finish the cancelled/closed read before dropping the fixture */
  g_object_unref (f->peer);
  g_object_unref (f->session);
  (void) data;
}

static void
make_packet (guint8 packet[11], guint nonce)
{
  guint8 key[32] = {0};
  ShannonCipher cipher;
  shannon_key_setup (&cipher, key, sizeof key);
  shannon_nonce_u32 (&cipher, nonce);
  packet[0] = AP_CMD_MERCURY_REQ;
  packet[1] = 0;
  packet[2] = 4;
  memcpy (packet + 3, "test", 4);
  shannon_encrypt (&cipher, packet, 7);
  shannon_finish (&cipher, packet + 7, 4);
}

static void
write_bytes (Fixture *f, const guint8 *bytes, gsize size)
{
  g_autoptr(GError) error = NULL;
  g_assert_true (g_output_stream_write_all (
    g_io_stream_get_output_stream (G_IO_STREAM (f->peer)), bytes, size,
    NULL, NULL, &error));
  g_assert_no_error (error);
}

static void
fragmented (Fixture *f, gconstpointer data)
{
  guint8 packet[11];
  make_packet (packet, 0);
  /* Each header byte arrives on a different main-loop turn. */
  for (guint i = 0; i < 3; i++) {
    write_bytes (f, packet + i, 1);
    pump (10);
    g_assert_cmpuint (f->disconnects, ==, 0);
    g_assert_cmpuint (f->packets, ==, 0);
  }
  /* The main loop remains responsive while an incomplete body is pending. */
  write_bytes (f, packet + 3, 2);
  pump (10);
  g_assert_cmpuint (f->packets, ==, 0);
  write_bytes (f, packet + 5, 6);
  pump (30);
  g_assert_cmpuint (f->packets, ==, 1);
  make_packet (packet, 1);
  write_bytes (f, packet, sizeof packet);
  pump (30);
  g_assert_cmpuint (f->packets, ==, 2);
  g_assert_cmpuint (f->session->recv_nonce, ==, 2);
  g_assert_cmpuint (f->disconnects, ==, 0);
  (void) data;
}

static void
truncated (Fixture *f, gconstpointer data)
{
  guint8 packet[11];
  make_packet (packet, 0);
  write_bytes (f, packet, GPOINTER_TO_UINT (data));
  g_io_stream_close (G_IO_STREAM (f->peer), NULL, NULL);
  pump (30);
  g_assert_cmpuint (f->packets, ==, 0);
  g_assert_cmpuint (f->disconnects, ==, 1);
  g_assert_false (f->session->connected);
}

static void
intentional_close (Fixture *f, gconstpointer data)
{
  spotifygtk_ap_session_disconnect (f->session);
  pump (30);
  g_assert_cmpuint (f->disconnects, ==, 0);
  g_assert_cmpuint (f->packets, ==, 0);
  (void) data;
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/ap/fragmented-header-and-body", Fixture, NULL, setup, fragmented, teardown);
  g_test_add ("/ap/truncated-header", Fixture, GUINT_TO_POINTER (1), setup, truncated, teardown);
  g_test_add ("/ap/truncated-body", Fixture, GUINT_TO_POINTER (5), setup, truncated, teardown);
  g_test_add ("/ap/intentional-close", Fixture, NULL, setup, intentional_close, teardown);
  return g_test_run ();
}

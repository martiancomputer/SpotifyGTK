/*
 * main.c — spotify-native development harness.
 *
 * Not a real client yet (no playback). What it does now:
 *
 *   1. Always: a Shannon cipher round-trip self-test (offline, no
 *      network) -- real signal that the ported algorithm is
 *      internally consistent.
 *
 *   2. If SPOTIFY_ACCESS_TOKEN is set in the environment: attempts a
 *      real end-to-end AP connection -- SRV resolve, TCP connect, DH
 *      handshake, then login using that OAuth token (the same kind
 *      of token apps/spotify-connect's auth.c already produces).
 *      This is the actual test of whether everything ported from
 *      librespot (handshake.rs, the shannon crate, authentication.rs)
 *      actually interoperates with Spotify's real servers -- which
 *      this development sandbox can't reach itself (ap.spotify.com
 *      isn't in its allowed network list), so this path only runs
 *      when a person with real network access sets the env var.
 *
 * Usage for step 2:
 *   export SPOTIFY_ACCESS_TOKEN="$(your already-working spotify-connect token)"
 *   ./spotify-native-harness
 */

#include "config.h"
#include "spotify/shannon.h"
#include "spotify/ap.h"

#include <glib.h>
#include <string.h>

static gboolean
run_shannon_selftest (void)
{
  /* nonstring: these are fixed-size byte arrays for the cipher, not
   * NUL-terminated C strings -- the string-literal initializer is
   * just a readable way to write 16/8 arbitrary bytes. */
  static const guint8 key[16]  __attribute__((nonstring)) = "0123456789abcdef";
  static const guint8 nonce[8] __attribute__((nonstring)) = "spotgtk!";
  const gchar *message   = "spotify-native engine harness -- shannon round-trip check";
  gsize        msg_len   = strlen (message);

  ShannonCipher sender, receiver;
  shannon_key_setup (&sender,   key, sizeof (key));
  shannon_key_setup (&receiver, key, sizeof (key));
  shannon_nonce (&sender,   nonce, sizeof (nonce));
  shannon_nonce (&receiver, nonce, sizeof (nonce));

  g_autofree guint8 *buf = g_malloc (msg_len);
  memcpy (buf, message, msg_len);

  shannon_encrypt (&sender, buf, msg_len);

  gboolean garbled_in_transit = (memcmp (buf, message, msg_len) != 0);

  shannon_decrypt (&receiver, buf, msg_len);

  gboolean roundtrip_ok = (memcmp (buf, message, msg_len) == 0);

  guint8 sender_mac[4], receiver_mac[4];
  shannon_finish (&sender,   sender_mac,   sizeof (sender_mac));
  shannon_finish (&receiver, receiver_mac, sizeof (receiver_mac));
  gboolean macs_match = (memcmp (sender_mac, receiver_mac, sizeof (sender_mac)) == 0);

  g_message ("[shannon-selftest] ciphertext != plaintext in transit: %s",
            garbled_in_transit ? "yes (expected)" : "NO -- cipher did nothing, something's wrong");
  g_message ("[shannon-selftest] decrypted message matches original: %s",
            roundtrip_ok ? "yes" : "NO -- round-trip failed");
  g_message ("[shannon-selftest] sender/receiver MACs agree: %s",
            macs_match ? "yes" : "NO -- MAC accumulation mismatch");

  return garbled_in_transit && roundtrip_ok && macs_match;
}

/* ── Live AP connection + login attempt (opt-in via env var) ───────────────── */

typedef struct {
  GMainLoop *loop;
  gboolean   ok;
} LiveTestState;

static void
on_login_result (gboolean success, const gchar *username, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;

  if (success) {
    g_message ("[live-test] LOGIN SUCCEEDED%s%s -- handshake, crypto, and login all verified "
              "against a real Spotify server.", username ? " as " : "", username ? username : "");
    state->ok = TRUE;
  } else {
    g_warning ("[live-test] login failed: %s", error ? error->message : "unknown error");
    state->ok = FALSE;
  }

  g_main_loop_quit (state->loop);
}

static void
on_connected (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyApSession *session = SPOTIFYGTK_AP_SESSION (source);
  LiveTestState     *state  = user_data;
  g_autoptr(GError)  err    = NULL;

  if (!spotifygtk_ap_session_connect_finish (session, result, &err)) {
    g_warning ("[live-test] handshake failed: %s", err ? err->message : "unknown error");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  g_message ("[live-test] handshake succeeded -- DH exchange, RSA signature verification, "
            "and HMAC key derivation all checked out against a real server.");

  spotifygtk_ap_session_start_receiving (session);

  const gchar *username = g_getenv ("SPOTIFY_USERNAME");  /* optional, may be NULL */
  const gchar *token     = g_getenv ("SPOTIFY_ACCESS_TOKEN");

  g_message ("[live-test] sending login...");
  spotifygtk_ap_session_login (session, username, token, on_login_result, state);
}

static gboolean
on_live_test_timeout (gpointer user_data)
{
  GMainLoop *loop = user_data;
  g_warning ("[live-test] timed out after 15s waiting for a response -- "
            "check network reachability to ap.spotify.com, or a firewall/proxy issue");
  g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}

static gboolean
run_live_test (void)
{
  const gchar *token = g_getenv ("SPOTIFY_ACCESS_TOKEN");

  g_message ("=== live AP connection test ===");
  g_message ("SPOTIFY_ACCESS_TOKEN is set -- attempting a real handshake + login "
            "against Spotify's actual AP service. This is the test that proves (or "
            "disproves) interop, which nothing offline can confirm.");

  SpotifyApSession *session = spotifygtk_ap_session_new ();
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  LiveTestState state = { .loop = loop, .ok = FALSE };

  spotifygtk_ap_session_connect (session, token, on_connected, &state);

  /* Bound how long we'll wait -- a hang here (e.g. SRV resolution
   * stalling, or a firewalled outbound connection) should fail
   * loudly rather than block forever.
   *
   * KNOWN LIMITATION, not fixed here: if this timeout fires, we tear
   * down `session` and return while an async GIO operation may still
   * be in flight (none of ap.c's async calls currently accept a
   * GCancellable -- they're hardcoded NULL throughout). That
   * in-flight callback would reference `state`, a stack variable
   * that's gone once this function returns. It's harmless in THIS
   * harness specifically (the process exits immediately after,
   * before the shared GMainContext is ever pumped again to deliver
   * the stale callback) -- but this exact pattern would be a real
   * use-after-free if reused inside a long-running app. Properly
   * fixing it means threading a GCancellable through every async hop
   * in ap.c (resolve, connect, handshake reads, receive loop) -- real
   * follow-up work, tracked rather than silently patched over here. */
  guint timeout_id = g_timeout_add_seconds (15, on_live_test_timeout, loop);

  g_main_loop_run (loop);

  g_source_remove (timeout_id);
  g_main_loop_unref (loop);
  g_object_unref (session);

  return state.ok;
}

int
main (int argc, char *argv[])
{
  (void) argc; (void) argv;

  g_message ("=== spotify-native engine harness (%s build) ===", APP_PROFILE);
  g_message ("    PipeWire: %s", HAVE_PIPEWIRE ? "yes" : "no");
  g_message ("    PulseAudio: %s", HAVE_PULSE ? "yes" : "no");
  g_message ("    ALSA: %s", HAVE_ALSA ? "yes" : "no");
  g_message ("    OpenSSL (CDN decrypt): %s", HAVE_OPENSSL ? "yes" : "no");
  g_message ("Not a real client yet -- no playback. See README.");

  gboolean shannon_ok = run_shannon_selftest ();
  if (!shannon_ok) {
    g_warning ("Shannon self-test FAILED -- see messages above");
    return 1;
  }
  g_message ("Shannon self-test passed.");

  if (g_getenv ("SPOTIFY_ACCESS_TOKEN")) {
    gboolean live_ok = run_live_test ();
    return live_ok ? 0 : 1;
  }

  g_message ("(set SPOTIFY_ACCESS_TOKEN to also attempt a real AP handshake + login)");
  return 0;
}

/*
 * main.c — spotify-native development harness.
 *
 * Not a real client yet (no playback). What it does now:
 *
 *   1. Always: a Shannon cipher round-trip self-test (offline, no
 *      network) -- real signal that the ported algorithm is
 *      internally consistent.
 *
 *   2. A real end-to-end AP connection attempt -- SRV resolve, TCP
 *      connect, DH handshake, then login. This is the actual test of
 *      whether everything ported from librespot (handshake.rs, the
 *      shannon crate, authentication.rs) interoperates with
 *      Spotify's real servers.
 *
 * Token source for step 2, in priority order:
 *
 *   a. SPOTIFY_ACCESS_TOKEN env var, if set -- manual override, e.g.
 *      for scripted/CI use with a token obtained some other way.
 *      IMPORTANT: this must be a token from native_auth's OWN OAuth
 *      flow (see below), not from apps/spotify-connect's auth.c.
 *      Those are genuinely different credentials -- a spotify-connect
 *      token was tested here and rejected by the AP within ~100ms,
 *      no structured error. See spotify/native_auth.h for exactly
 *      why (different client_id, different scope list -- verified
 *      against librespot's own source, not guessed).
 *
 *   b. A previously stored native_auth token (~/.config/spotify-native/token),
 *      refreshed automatically if expired but refreshable.
 *
 *   c. Otherwise: opens your browser for Spotify's consent screen
 *      using native_auth's keymaster-client_id PKCE flow. Complete
 *      the login there; this process picks it up automatically and
 *      continues once you do.
 *
 * Usage:
 *   ./spotify-native-harness
 *   (first run opens a browser; subsequent runs reuse the stored
 *   token silently until it needs a refresh or you revoke it)
 */

#include "config.h"
#include "spotify/shannon.h"
#include "spotify/ap.h"
#include "spotify/native_auth.h"

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

/* ── Token acquisition via native_auth (keymaster client_id) ────────────────
 * Own GMainLoop, own indefinite wait -- this step involves an actual
 * human clicking through a browser consent screen, so it deliberately
 * has no short timeout the way the AP connection phase does below. */

typedef struct {
  GMainLoop *loop;
  gboolean   ok;
} TokenAcquireState;

static void
on_native_auth_completed (NativeAuth *auth, gboolean success, gpointer user_data)
{
  (void) auth;
  TokenAcquireState *state = user_data;
  state->ok = success;
  g_main_loop_quit (state->loop);
}

/* Returns a newly-allocated copy of the token (caller frees), or NULL
 * on failure. Blocks (via its own main loop) until resolved. */
static gchar *
acquire_native_token (void)
{
  NativeAuth *auth = native_auth_new ();

  if (native_auth_has_valid_token (auth)) {
    g_message ("[auth] using previously stored native-flow token");
    gchar *token = g_strdup (native_auth_get_token (auth));
    g_object_unref (auth);
    return token;
  }

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  TokenAcquireState state = { .loop = loop, .ok = FALSE };
  g_signal_connect (auth, "completed", G_CALLBACK (on_native_auth_completed), &state);

  /* native_auth_refresh() does the right thing either way: if a
   * refresh_token was loaded from a stored-but-expired token file, it
   * POSTs a refresh; if there's no stored token at all, it falls
   * through to native_auth_begin() (the browser flow) itself. */
  g_message ("[auth] no valid stored token -- attempting refresh or fresh login...");
  native_auth_refresh (auth);

  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  gchar *token = state.ok ? g_strdup (native_auth_get_token (auth)) : NULL;
  g_object_unref (auth);
  return token;
}

/* ── Live AP connection + login attempt ──────────────────────────────────── */

typedef struct {
  GMainLoop *loop;
  gboolean   ok;
  gboolean   timed_out;
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
  const gchar *token    = g_object_get_data (G_OBJECT (session), "login-token");

  g_message ("[live-test] sending login...");
  spotifygtk_ap_session_login (session, username, token, on_login_result, state);
}

static gboolean
on_live_test_timeout (gpointer user_data)
{
  LiveTestState *state = user_data;
  g_warning ("[live-test] timed out after 15s waiting for a response -- "
            "check network reachability to ap.spotify.com, or a firewall/proxy issue");
  state->timed_out = TRUE;
  g_main_loop_quit (state->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
run_live_test (const gchar *token)
{
  g_message ("=== live AP connection test ===");
  g_message ("Attempting a real handshake + login against Spotify's actual AP "
            "service. This is the test that proves (or disproves) interop, "
            "which nothing offline can confirm.");

  SpotifyApSession *session = spotifygtk_ap_session_new ();
  /* Stashed here rather than threaded through as a separate callback
   * parameter -- on_connected() needs it and is reached via
   * spotifygtk_ap_session_connect()'s fixed GAsyncReadyCallback
   * signature, which only carries `session` and our own user_data
   * (already used for `state`). g_object_set_data() is the simplest
   * way to attach a second piece of data to the same object without
   * inventing a wrapper struct just for this. */
  g_object_set_data_full (G_OBJECT (session), "login-token", g_strdup (token), g_free);

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  LiveTestState state = { .loop = loop, .ok = FALSE, .timed_out = FALSE };

  spotifygtk_ap_session_connect (session, NULL, on_connected, &state);

  /* Bound how long we'll wait -- a hang here (e.g. SRV resolution
   * stalling, or a firewalled outbound connection) should fail
   * loudly rather than block forever. This is now mostly a true
   * network-hang detector: a clean rejection (bad token, etc.) fails
   * fast via ap.c's "disconnected" signal rather than waiting out
   * this timeout, so reaching it specifically suggests a stuck
   * connection, not a normal login rejection.
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
   * in ap.c (resolve, connect, handshake reads, receive loop): real
   * follow-up work, tracked rather than silently patched over here. */
  guint timeout_id = g_timeout_add_seconds (15, on_live_test_timeout, &state);

  g_main_loop_run (loop);

  /* on_live_test_timeout() returns G_SOURCE_REMOVE, which tells GLib
   * to auto-remove that source the moment it fires -- calling
   * g_source_remove() again here unconditionally was a real bug
   * (GLib-CRITICAL: "Source ID N was not found") whenever the
   * timeout was what ended the loop. Only remove it ourselves when
   * something else (success or failure callback) ended the loop
   * first, leaving the timeout source still pending. */
  if (!state.timed_out)
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

  const gchar *env_token = g_getenv ("SPOTIFY_ACCESS_TOKEN");
  g_autofree gchar *acquired_token = NULL;
  const gchar *token = NULL;

  if (env_token && *env_token) {
    g_message ("[auth] using SPOTIFY_ACCESS_TOKEN from environment "
              "(make sure this came from native_auth's own flow, not spotify-connect's)");
    token = env_token;
  } else {
    acquired_token = acquire_native_token ();
    token = acquired_token;
  }

  if (!token) {
    g_warning ("Could not obtain an access token -- see messages above. Skipping live test.");
    return 1;
  }

  gboolean live_ok = run_live_test (token);
  return live_ok ? 0 : 1;
}

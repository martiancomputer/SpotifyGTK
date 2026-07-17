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
 *   3. On a successful AP login: chains into the streaming auth-relay
 *      (client-token, then login5) using the reusable credential
 *      APWelcome handed back -- NOT the original OAuth token. This is
 *      the piece discovered while researching actual playback: track
 *      metadata and CDN URL resolution go through a separate HTTPS
 *      API (spclient), which needs a login5-minted bearer token, which
 *      itself needs this reusable credential. See spotify/login5.h
 *      and research/playback/ for the full finding.
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
#include "spotify/clienttoken.h"
#include "spotify/login5.h"
#include "spotify/spclient.h"
#include "spotify/audio_key.h"
#include "spotify/cdn.h"

#include <glib.h>
#include <string.h>

/* Generate a random 40-char hex device ID (same pattern as connect.c).
 * Per-run is fine for a harness -- persisting it is a nice-to-have
 * for a real client so "this device" stays recognised across restarts. */
static gchar *
generate_device_id (void)
{
  guint8 raw[20];
  GRand *r = g_rand_new ();
  for (int i = 0; i < 20; i++) raw[i] = (guint8) g_rand_int_range (r, 0, 256);
  g_rand_free (r);
  GString *hex = g_string_new (NULL);
  for (int i = 0; i < 20; i++) g_string_append_printf (hex, "%02x", raw[i]);
  return g_string_free (hex, FALSE);
}

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
  gchar             *device_id;         /* owned, freed in run_live_test */
  SpotifyApSession   *session;      /* borrowed, owned by run_live_test */
  SpotifyClientToken *client_token_client;
  SpotifyLogin5      *login5_client;
  SpotifySpclient    *spclient;
  SpotifyAudioKeyClient *audio_key_client;
  SpotifyCdnFetcher  *cdn_fetcher;
  gchar              *bearer_token;
  gchar              *client_token;
  guint8              file_id[20];
  guint8              track_gid[16];
  gchar              *cdn_url;
} LiveTestState;

static void
on_aes_key (SpotifyApSession *session, ApCommandId cmd, const guint8 *payload, gsize len, gpointer user_data)
{
  LiveTestState *state = user_data;
  g_message ("[live-test] received AP_CMD_AES_KEY (%" G_GSIZE_FORMAT " bytes); dispatching to audio-key client", len);
  spotifygtk_audio_key_handle_response (state->audio_key_client, payload, len);
}

static void
on_aes_key_error (SpotifyApSession *session, ApCommandId cmd, const guint8 *payload, gsize len, gpointer user_data)
{
  LiveTestState *state = user_data;
  g_warning ("[live-test] received AP_CMD_AES_KEY_ERROR (%" G_GSIZE_FORMAT " bytes); dispatching to audio-key client", len);
  spotifygtk_audio_key_handle_error (state->audio_key_client, payload, len);
}

static void
on_cdn_chunk_result (GBytes *decrypted_chunk, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;

  if (decrypted_chunk) {
    gsize len = 0;
    const guint8 *data = g_bytes_get_data (decrypted_chunk, &len);
    g_message ("[live-test] CDN DECRYPT SUCCEEDED -- decrypted %" G_GSIZE_FORMAT " bytes.", len);
    if (len >= 4) {
      g_message ("[live-test] First 4 bytes: %02x %02x %02x %02x (expected 4f 67 67 53 for 'OggS')",
                 data[0], data[1], data[2], data[3]);
      if (memcmp (data, "OggS", 4) == 0) {
        g_message ("[live-test] SUCCESS: Decrypted chunk has a valid Ogg stream header!");
        state->ok = TRUE;
      } else {
        g_warning ("[live-test] FAILURE: Decrypted chunk does not start with OggS. "
                   "AES IV/key/offset is incorrect.");
        state->ok = FALSE;
      }
    } else {
      g_warning ("[live-test] Decrypted chunk is too small.");
      state->ok = FALSE;
    }
  } else {
    g_warning ("[live-test] CDN fetch failed: %s", error ? error->message : "unknown error");
    state->ok = FALSE;
  }

  g_main_loop_quit (state->loop);
}

static void
on_audio_key_result (const guint8 key[AUDIO_KEY_LEN], GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;

  if (key) {
    g_message ("[live-test] AUDIO KEY SUCCEEDED -- got 16-byte AES key.");
    g_message ("[live-test] Fetching and decrypting CDN chunk (offset 0 -> physical 167)...");
    spotifygtk_cdn_fetch_chunk (state->cdn_fetcher, state->cdn_url, key,
                                0, 16384, on_cdn_chunk_result, state);
    return; /* Wait for CDN chunk */
  } else {
    g_warning ("[live-test] audio key request failed: %s", error ? error->message : "unknown error");
    state->ok = FALSE;
  }

  g_main_loop_quit (state->loop);
}

static void
on_audio_storage_result (SpclientCdnUrls *urls, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;

  if (urls && urls->n_urls > 0) {
    g_message ("[live-test] STORAGE RESOLUTION SUCCEEDED -- CDN URLs:");
    for (guint i = 0; i < urls->n_urls; i++)
      g_message ("  %s", urls->urls[i]);
    
    state->cdn_url = g_strdup (urls->urls[0]);
    spclient_cdn_urls_free (urls);

    g_message ("[live-test] Requesting audio key for the track...");
    spotifygtk_audio_key_request (state->audio_key_client, state->track_gid, 16,
                                  state->file_id, 20, on_audio_key_result, state);
    return; /* Wait for AES key */
  } else {
    g_warning ("[live-test] get_audio_storage failed: %s", error ? error->message : "unknown error");
    state->ok = FALSE;
  }

  g_main_loop_quit (state->loop);
}

static void
on_track_metadata_result (const SpclientAudioFile *files, guint n_files, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;

  if (files) {
    g_message ("[live-test] TRACK METADATA SUCCEEDED -- found %u files:", n_files);
    gint best_idx = -1;
    for (guint i = 0; i < n_files; i++) {
      g_message ("  file[%u]: format=%" G_GUINT64_FORMAT, i, files[i].format);
      
      /* OGG_VORBIS_320 = 2, OGG_VORBIS_160 = 1, OGG_VORBIS_96 = 0 */
      if (files[i].format == 2) best_idx = i;
      else if (best_idx == -1 && files[i].format == 1) best_idx = i;
      else if (best_idx == -1 && files[i].format == 0) best_idx = i;
    }
    
    if (best_idx != -1) {
      g_message ("[live-test] Selected format %" G_GUINT64_FORMAT ". Resolving CDN URLs...", files[best_idx].format);
      
      memcpy (state->file_id, files[best_idx].file_id, 20);
      memcpy (state->track_gid, files[best_idx].track_gid, 16);
      
      spotifygtk_spclient_get_audio_storage (state->spclient,
                                             files[best_idx].file_id, 20,
                                             state->bearer_token, state->client_token,
                                             on_audio_storage_result, state);
      return; /* Wait for storage_resolve */
    } else {
      g_warning ("[live-test] No suitable OGG_VORBIS file found.");
      state->ok = FALSE;
    }
  } else {
    g_warning ("[live-test] get_track_metadata failed: %s", error ? error->message : "unknown error");
    state->ok = FALSE;
  }

  g_main_loop_quit (state->loop);
}

static void
on_login5_result (const gchar *access_token, gint32 expires_in_seconds,
                  GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;

  if (access_token) {
    g_message ("[live-test] LOGIN5 SUCCEEDED -- obtained a spclient-usable bearer token "
              "(expires in %ds).", expires_in_seconds);
    
    state->bearer_token = g_strdup (access_token);
    state->spclient = spotifygtk_spclient_new ();
    
    const gchar *test_track = "spotify:track:6rqhFgbbKwnb9MLmUQDhG6";
    g_message ("[live-test] Fetching metadata for %s...", test_track);
    spotifygtk_spclient_get_track_metadata (state->spclient, test_track,
                                            state->bearer_token, state->client_token,
                                            on_track_metadata_result, state);
    return; /* Wait for track metadata */
  } else {
    g_warning ("[live-test] login5 failed: %s", error ? error->message : "unknown error");
    g_warning ("[live-test] AP login itself succeeded -- only the streaming auth-relay "
              "chain (client-token/login5) failed. See spotify/login5.h and spotify/"
              "clienttoken.h.");
    state->ok = FALSE;
  }

  g_main_loop_quit (state->loop);
}

static void
on_client_token_result (const gchar *token, gpointer user_data)
{
  LiveTestState *state = user_data;

  if (token) {
    g_message ("[live-test] client-token obtained, proceeding to login5...");
    state->client_token = g_strdup (token);
  } else {
    g_message ("[live-test] no client-token obtained -- proceeding to login5 without one "
              "(login5 requires one per spclient.rs; expect this to fail if so)");
  }

  const gchar *username      = spotifygtk_ap_session_get_username (state->session);
  gsize        creds_len     = 0;
  const guint8 *creds        = spotifygtk_ap_session_get_reusable_creds (state->session, &creds_len);
  guint64      creds_type    = spotifygtk_ap_session_get_reusable_creds_type (state->session);

  if (!creds) {
    g_warning ("[live-test] no reusable_auth_credentials captured from APWelcome -- "
              "cannot proceed to login5");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  state->login5_client = spotifygtk_login5_new ();
  spotifygtk_login5_auth_token (state->login5_client,
                                NATIVE_AUTH_CLIENT_ID, state->device_id,
                                username, creds, creds_len, creds_type,
                                token, on_login5_result, state);
}

static void
on_login_result (gboolean success, const gchar *username, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;

  if (!success) {
    g_warning ("[live-test] login failed: %s", error ? error->message : "unknown error");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  g_message ("[live-test] LOGIN SUCCEEDED%s%s -- handshake, crypto, and login all verified "
            "against a real Spotify server.", username ? " as " : "", username ? username : "");

  /* Chain into the streaming auth-relay: client-token, then login5,
   * using the reusable credential APWelcome handed back (NOT the
   * original native_auth OAuth token -- see spotify/login5.h). This
   * is the next real unknown worth proving against actual Spotify
   * services rather than just building and hoping. */
  g_message ("[live-test] AP login confirmed -- now testing the streaming auth-relay chain "
            "(client-token -> login5)...");
  state->client_token_client = spotifygtk_client_token_new ();
  spotifygtk_client_token_request (state->client_token_client, NATIVE_AUTH_CLIENT_ID,
                                   state->device_id,
                                   on_client_token_result, state);
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

  state->session = session;
  spotifygtk_ap_session_start_receiving (session);
  state->audio_key_client = spotifygtk_audio_key_client_new (session);
  state->cdn_fetcher = spotifygtk_cdn_fetcher_new ();

  /* `state` is already the LiveTestState pointer supplied by run_live_test().
   * Passing `&state` here would instead retain a pointer to this callback's
   * stack-local parameter; the first audio-key reply would dereference that
   * invalid address and crash. */
  spotifygtk_ap_session_set_handler (session, AP_CMD_AES_KEY, on_aes_key, state);
  spotifygtk_ap_session_set_handler (session, AP_CMD_AES_KEY_ERROR, on_aes_key_error, state);
  g_message ("[live-test] registered AP audio-key response handlers");

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
  LiveTestState state = {
    .loop = loop, .ok = FALSE, .timed_out = FALSE,
    .device_id = generate_device_id (),
    .session = NULL, .client_token_client = NULL, .login5_client = NULL,
    .spclient = NULL, .audio_key_client = NULL, .cdn_fetcher = NULL,
    .bearer_token = NULL, .client_token = NULL, .cdn_url = NULL,
  };
  g_message ("[live-test] device_id for this run: %s", state.device_id);

  spotifygtk_ap_session_connect (session, NULL, on_connected, &state);

  /* Bound how long we'll wait -- a hang here (e.g. SRV resolution
   * stalling, or a firewalled outbound connection) should fail
   * loudly rather than block forever. This is now mostly a true
   * network-hang detector: a clean rejection (bad token, etc.) fails
   * fast via ap.c's "disconnected" signal rather than waiting out
   * this timeout, so reaching it specifically suggests a stuck
   * connection, not a normal login rejection. Raised from 15s to 25s
   * now that a successful AP login chains into two more real network
   * round trips (client-token, login5) before the test concludes.
   *
   * KNOWN LIMITATION, not fixed here: if this timeout fires, we tear
   * down `session` (and now also client_token_client/login5_client if
   * created) while an async GIO operation may still be in flight
   * (none of ap.c's, clienttoken.c's, or login5.c's async calls
   * currently accept a GCancellable -- they're hardcoded NULL
   * throughout). That in-flight callback would reference `state`, a
   * stack variable that's gone once this function returns. It's
   * harmless in THIS harness specifically (the process exits
   * immediately after, before the shared GMainContext is ever pumped
   * again to deliver the stale callback) -- but this exact pattern
   * would be a real use-after-free if reused inside a long-running
   * app. Properly fixing it means threading a GCancellable through
   * every async hop across all three files: real follow-up work,
   * tracked rather than silently patched over here. */
  guint timeout_id = g_timeout_add_seconds (25, on_live_test_timeout, &state);

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
  g_free (state.device_id);
  g_clear_object (&state.client_token_client);
  g_clear_object (&state.login5_client);
  g_clear_object (&state.spclient);
  g_clear_object (&state.audio_key_client);
  g_clear_object (&state.cdn_fetcher);
  g_free (state.bearer_token);
  g_free (state.client_token);
  g_free (state.cdn_url);
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

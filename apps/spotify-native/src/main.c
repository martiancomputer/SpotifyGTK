/*
 * main.c — spotify-native development harness.
 *
 * Development harness for the native playback pipeline. It now exercises
 * authenticated CDN fetch, decryption, Ogg/Vorbis decode, and local PCM
 * output; the GTK shell drives this same engine asynchronously.
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
#include "spotify/session.h"
#include "spotify/audio_key.h"
#include "spotify/cdn.h"
#include "spotify/mercury.h"
#include "spotify/collection.h"
#include "spotify/protobuf_min.h"
#include "audio/decoder.h"
#include "audio/dsp.h"
#include "audio/resampler.h"
#include "audio/output.h"
#include "native_engine.h"

#include <glib.h>
#include <string.h>

struct _SpotifyNativeEngineControl {
  GMutex  lock;
  GCond   cond;
  gboolean paused;
  gdouble  volume;         /* 0.0 - 1.0 */
  gboolean volume_dirty;   /* set by the UI, cleared once the output applies it */
  guint64  played_frames;  /* cumulative PCM frames written to the device */
  gint     sample_rate;    /* of the stream being played; 0 until known */
  gboolean seek_requested; /* UI asked to reposition */
  gint64   seek_target_ms; /* where to; valid while seek_requested */

  /* Equaliser. Gains (dB) and enable are pushed from the UI like volume;
   * the filter itself lives here so its state persists across buffers. */
  SpotifyEq *eq;
  gdouble    eq_gains[SPOTIFYGTK_EQ_BANDS];
  gboolean   eq_enabled;
  gboolean   eq_dirty;

  /* Desired device rate; 0 means "follow whatever the stream decoded to",
   * which is passthrough and the highest-fidelity path. */
  gint       output_rate;
};

SpotifyNativeEngineControl *
spotifygtk_native_engine_control_new (void)
{
  SpotifyNativeEngineControl *control = g_new0 (SpotifyNativeEngineControl, 1);
  g_mutex_init (&control->lock);
  g_cond_init (&control->cond);
  control->volume = 1.0;
  control->volume_dirty = TRUE;   /* apply once as soon as the output opens */
  control->eq = spotifygtk_eq_new ();
  return control;
}

void
spotifygtk_native_engine_control_free (SpotifyNativeEngineControl *control)
{
  if (!control)
    return;
  spotifygtk_native_engine_control_resume (control);
  spotifygtk_eq_free (control->eq);
  g_mutex_clear (&control->lock);
  g_cond_clear (&control->cond);
  g_free (control);
}

void
spotifygtk_native_engine_control_pause (SpotifyNativeEngineControl *control)
{
  if (!control)
    return;
  g_mutex_lock (&control->lock);
  control->paused = TRUE;
  g_mutex_unlock (&control->lock);
  g_message ("engine-control: PCM output paused");
}

void
spotifygtk_native_engine_control_resume (SpotifyNativeEngineControl *control)
{
  if (!control)
    return;
  g_mutex_lock (&control->lock);
  control->paused = FALSE;
  g_cond_broadcast (&control->cond);
  g_mutex_unlock (&control->lock);
  g_message ("engine-control: PCM output resumed");
}

gboolean
spotifygtk_native_engine_control_is_paused (SpotifyNativeEngineControl *control)
{
  if (!control)
    return FALSE;
  g_mutex_lock (&control->lock);
  gboolean paused = control->paused;
  g_mutex_unlock (&control->lock);
  return paused;
}

void
spotifygtk_native_engine_control_set_volume (SpotifyNativeEngineControl *control,
                                             gdouble volume_0_to_1)
{
  if (!control)
    return;
  g_mutex_lock (&control->lock);
  control->volume = CLAMP (volume_0_to_1, 0.0, 1.0);
  control->volume_dirty = TRUE;
  g_mutex_unlock (&control->lock);
}

void
spotifygtk_native_engine_control_set_eq (SpotifyNativeEngineControl *control,
                                         const gdouble *gains_db, gboolean enabled)
{
  if (!control)
    return;
  g_mutex_lock (&control->lock);
  if (gains_db)
    memcpy (control->eq_gains, gains_db, sizeof control->eq_gains);
  control->eq_enabled = enabled;
  control->eq_dirty = TRUE;
  g_mutex_unlock (&control->lock);
}

/* Apply the equaliser to a PCM buffer in the audio worker, after volume.
 * Pushes any pending gain change into the filter, then filters in place. */
void
spotifygtk_native_engine_control_set_output_rate (SpotifyNativeEngineControl *control,
                                                  gint rate_hz)
{
  if (!control) return;
  g_mutex_lock (&control->lock);
  control->output_rate = rate_hz > 0 ? rate_hz : 0;
  g_mutex_unlock (&control->lock);
}

static gint
engine_control_get_output_rate (SpotifyNativeEngineControl *control)
{
  if (!control) return 0;
  g_mutex_lock (&control->lock);
  gint r = control->output_rate;
  g_mutex_unlock (&control->lock);
  return r;
}

static void
engine_control_apply_eq (SpotifyNativeEngineControl *control,
                         gint16 *samples, gsize n_frames, gint channels, gint rate)
{
  if (!control || !control->eq)
    return;

  g_mutex_lock (&control->lock);
  gboolean dirty   = control->eq_dirty;
  gboolean enabled = control->eq_enabled;
  gdouble  gains[SPOTIFYGTK_EQ_BANDS];
  memcpy (gains, control->eq_gains, sizeof gains);
  control->eq_dirty = FALSE;
  g_mutex_unlock (&control->lock);

  if (dirty)
    spotifygtk_eq_set (control->eq, gains, enabled);

  spotifygtk_eq_process (control->eq, samples, n_frames, channels, rate);
}

gdouble
spotifygtk_native_engine_control_get_volume (SpotifyNativeEngineControl *control)
{
  if (!control)
    return 1.0;
  g_mutex_lock (&control->lock);
  gdouble volume = control->volume;
  g_mutex_unlock (&control->lock);
  return volume;
}

void
spotifygtk_native_engine_control_report_position (SpotifyNativeEngineControl *control,
                                                  guint64 played_frames, gint sample_rate)
{
  if (!control)
    return;
  g_mutex_lock (&control->lock);
  control->played_frames = played_frames;
  control->sample_rate   = sample_rate;
  g_mutex_unlock (&control->lock);
}

gint64
spotifygtk_native_engine_control_get_position_ms (SpotifyNativeEngineControl *control)
{
  if (!control)
    return 0;
  g_mutex_lock (&control->lock);
  gint64 ms = control->sample_rate > 0
    ? (gint64) (control->played_frames * 1000u / (guint64) control->sample_rate)
    : 0;
  g_mutex_unlock (&control->lock);
  return ms;
}

void
spotifygtk_native_engine_control_request_seek (SpotifyNativeEngineControl *control,
                                               gint64 target_ms)
{
  if (!control)
    return;
  g_mutex_lock (&control->lock);
  control->seek_requested = TRUE;
  control->seek_target_ms = MAX (target_ms, 0);
  /* Wake anything blocked on the pause/queue condition so the engine notices
   * the request promptly rather than at the next 100ms poll only. */
  g_cond_broadcast (&control->cond);
  g_mutex_unlock (&control->lock);
}

gboolean
spotifygtk_native_engine_control_seek_pending (SpotifyNativeEngineControl *control)
{
  if (!control)
    return FALSE;
  g_mutex_lock (&control->lock);
  gboolean pending = control->seek_requested;
  g_mutex_unlock (&control->lock);
  return pending;
}

gboolean
spotifygtk_native_engine_control_take_seek (SpotifyNativeEngineControl *control,
                                            gint64 *out_target_ms)
{
  if (!control)
    return FALSE;
  g_mutex_lock (&control->lock);
  gboolean pending = control->seek_requested;
  if (pending) {
    if (out_target_ms)
      *out_target_ms = control->seek_target_ms;
    control->seek_requested = FALSE;
  }
  g_mutex_unlock (&control->lock);
  return pending;
}

/*
 * Apply the current gain to a PCM buffer in place.
 *
 * Every output backend declares `.set_volume = NULL` -- Pulse, ALSA and
 * PipeWire all leave per-stream volume as a TODO -- so routing the slider
 * to spotifygtk_output_set_volume() reached a stub and did nothing. Scaling
 * the samples ourselves works on every backend and needs nothing from them,
 * which is the same "pure-C fallback" rule the rest of the project follows.
 *
 * The curve is cubic rather than linear. Perceived loudness is roughly
 * logarithmic, so a linear slider spends most of its travel sounding loud
 * and then collapses at the bottom; cubing approximates the usual
 * fader response closely enough and costs one multiply.
 *
 * Samples are gint16, so the product is computed in gint32 and clamped:
 * gain is <= 1.0 here, but clamping keeps this correct if it ever isn't.
 */
static void
engine_control_apply_volume (SpotifyNativeEngineControl *control,
                             gint16 *samples, gsize n_frames, gint channels)
{
  if (!control || !samples || n_frames == 0)
    return;

  g_mutex_lock (&control->lock);
  gdouble volume = control->volume;
  control->volume_dirty = FALSE;
  g_mutex_unlock (&control->lock);

  if (volume >= 1.0)
    return;                       /* unity: leave the buffer untouched */

  gdouble gain = volume * volume * volume;
  gsize   n_samples = n_frames * (gsize) MAX (channels, 1);

  if (gain <= 0.0) {
    memset (samples, 0, n_samples * sizeof *samples);
    return;
  }

  for (gsize i = 0; i < n_samples; i++) {
    gint32 scaled = (gint32) (samples[i] * gain);
    samples[i] = (gint16) CLAMP (scaled, G_MININT16, G_MAXINT16);
  }
}

static gboolean
engine_control_wait (SpotifyNativeEngineControl *control, GCancellable *cancellable)
{
  gboolean cancelled = cancellable && g_cancellable_is_cancelled (cancellable);
  if (!control)
    return !cancelled;

  g_mutex_lock (&control->lock);
  while (control->paused && !(cancellable && g_cancellable_is_cancelled (cancellable)))
    g_cond_wait_until (&control->cond, &control->lock,
                       g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND);
  gboolean allowed = !control->paused &&
                     !(cancellable && g_cancellable_is_cancelled (cancellable));
  g_mutex_unlock (&control->lock);
  return allowed;
}

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
  GCancellable      *cancellable;   /* borrowed from the player task */
  gchar             *device_id;         /* owned, freed in run_live_test */
  guint              connect_attempts;  /* AP connect retries used so far */
  SpotifyApSession  *pending_session;   /* borrowed; set before the first connect */
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
  guint8              audio_key[AUDIO_KEY_LEN];
  GSource            *key_timeout_source;
  /* Read-ahead retries spent on the current position. Reset on every chunk
   * that lands, so this bounds consecutive failures rather than the track. */
  guint               read_ahead_retries;
  gchar              *cdn_url;
  const gchar         *track_uri;
  GBytes             *initial_cdn_chunk;
  goffset             next_download_offset;
  /* Seek support: the Ogg header pages (id/comment/setup) cached from the
   * start of the stream, replayed into a fresh decoder on every seek so it can
   * decode from a mid-file page; and a generation counter that invalidates a
   * read-ahead fetch still in flight when a seek supersedes it. */
  guint8             *header_prefix;
  gsize               header_prefix_len;
  guint               playback_generation;
  /* Frames to discard after a seek lands, to close the gap between the Ogg
   * page we landed on and the sample actually asked for. See do_seek. */
  guint64             seek_discard_frames;
  /* Rate the output device is actually running at, which is NOT the stream
   * rate once the resampler is engaged. Written by the audio worker when it
   * opens the device, read by the seek landing; a plain int written once per
   * output open, so the race is benign. */
  gint                device_rate;
  glong               bitrate_nominal;   /* for the seek byte estimate */
  gint                stream_sample_rate;
  SpotifyDecoder     *stream_decoder;
  guint64             stream_frames;
  guint64             output_frames;
  GAsyncQueue        *pcm_queue;
  GThread             *output_thread;
  GMutex               queue_lock;
  GCond                queue_cond;
  guint64              queued_frames;
  gboolean              output_failed;
  gboolean              queue_initialized;
  GSource            *timeout_source;
  GSource            *cancel_source;   /* quits the loop when the UI cancellable trips */
  GMainContext       *context;         /* borrowed: the engine's private context */
  SpotifyNativeEngineControl *control;
  SpotifyNativeEngineProgressFunc progress;
  gpointer            progress_data;
} LiveTestState;

/*
 * The run currently owning the engine, or NULL between tracks.
 *
 * LiveTestState is a stack local in engine_run, and async callbacks carry it as
 * user_data. That was safe while the context died with the run -- pending
 * callbacks died with it. Now the context persists, so a response arriving
 * after a run has returned lands on a dead stack frame. It crashed exactly
 * that way in on_audio_storage_result: a storage resolve that succeeded after
 * its run had ended, dereferencing a freed audio_key_client.
 *
 * Draining before returning narrows that window but cannot close it, because a
 * request that is stuck rather than cancellable never completes to be drained
 * -- which is precisely what the CDN deadline work found earlier today.
 *
 * So callbacks compare their state pointer against this before touching it.
 * Comparing a dangling pointer is well defined; dereferencing one is not, and
 * this check happens before any dereference.
 *
 * Every callback reached through a persistent object needs this, not just the
 * ones handling audio. A crash was traced to on_login_result running with a
 * previous run's state: the AP session outlives a track, so its login callback
 * still pointed at a LiveTestState whose stack frame belonged to an engine
 * thread that had already exited. state->progress read as 0x1 and the jump
 * through it took the process down. The guard is cheap, so the rule is that
 * anything taking LiveTestState from user_data checks first.
 *
 * The one exception is audio_output_thread, which the run creates and joins,
 * so it cannot outlive the state it was handed. It is not an oversight.
 *
 * Publishing order makes this safe: engine_active_state is assigned before any
 * connect or fetch is kicked off, so a callback belonging to the live run
 * always passes and only a stale one is dropped.
 */
static LiveTestState *engine_active_state = NULL;

static inline gboolean
run_is_current (LiveTestState *state)
{
  return state != NULL && state == engine_active_state;
}


/* Deliberately not AES-block aligned: comparing this independently
 * decrypted range with the same bytes from the initial range proves both
 * counter advancement and the intra-block discard path in cdn.c. */
/* How far short of the seek target to aim, and the largest gap we will close
 * by decoding and throwing away. Beyond that the byte estimate was wrong
 * enough that trimming would mean discarding real seconds of audio. */
#define SEEK_UNDERSHOOT_MS   1500
/* Past this, the byte estimate was wrong enough that trimming would mean
 * decoding away real seconds of audio -- so re-aim using the ratio we just
 * measured instead, once, and trim the remainder of that. */
#define SEEK_REFINE_SEC      4
/* Absolute ceiling on trimming, purely so a pathological granule cannot make
 * the engine sit there discarding forever. */
#define SEEK_MAX_TRIM_SEC    60

#define CDN_SEEK_PROBE_OFFSET 4093
#define CDN_SEEK_PROBE_LENGTH 8192
/*
 * Buffering depth.
 *
 * The first fetch stays small so playback starts promptly -- it is the only
 * thing standing between pressing play and hearing audio.
 *
 * Read-ahead chunks are larger, because after that the goal is the opposite:
 * get ahead of the playhead. At 320kbps a 64KB chunk is about 1.6 seconds of
 * audio, so the old settings kept roughly one and a half seconds of runway and
 * had to be back on the network constantly. Any hiccup longer than that was
 * audible, and a resume from pause landed immediately on a request.
 *
 * The PCM queue is what actually decides the depth: the fetch-decode loop runs
 * until the queue is full and then throttles, so raising it is what lets the
 * downloader run further ahead. Thirty seconds of 16-bit stereo at 44.1kHz is
 * about 5 MB -- cheap against the process, and enough that a resume plays for
 * half a minute before needing the network, which covers a stale connection,
 * the CDN request deadline and a retry with room to spare.
 */
#define CDN_DECODE_PROBE_LENGTH (64 * 1024)
#define CDN_FULL_DOWNLOAD_CHUNK (256 * 1024)
#define STREAM_QUEUE_MAX_FRAMES (44100 * 30)
#define DEFAULT_TEST_TRACK_URI "spotify:track:6rqhFgbbKwnb9MLmUQDhG6"

typedef struct {
  PcmFrame *frame;
  gboolean  end;
} StreamQueueItem;

static void
report_progress (LiveTestState *state, SpotifyNativeEngineStage stage,
                 const gchar *message)
{
  if (state->progress)
    state->progress (stage, message, state->progress_data);
}

static gboolean
decode_and_play (GBytes *ogg_bytes, gboolean complete_source,
                 gboolean write_audio, const gchar *label)
{
  SpotifyDecoder *decoder = spotifygtk_decoder_new ();
  if (complete_source) {
    if (!spotifygtk_decoder_open_complete (decoder, ogg_bytes)) {
      g_object_unref (decoder);
      return FALSE;
    }
  } else {
    spotifygtk_decoder_feed (decoder, ogg_bytes);
  }

  guint64 total_frames = 0;
  guint64 written_frames = 0;
  guint packets = 0;
  gint sample_rate = 0;
  gint channels = 0;
  SpotifyAudioOutput *output = NULL;
  PcmFrame *frame = NULL;
  while ((frame = spotifygtk_decoder_pull (decoder)) != NULL) {
    if (packets == 0) {
      sample_rate = frame->sample_rate;
      channels = frame->channels;
      g_message ("[live-test] decoder opened Ogg/Vorbis: %d Hz, %d channel(s)",
                 sample_rate, channels);
      if (write_audio) {
        output = spotifygtk_output_open (sample_rate, channels);
        if (!output) {
          g_warning ("[live-test] cannot complete PCM output proof: no audio backend opened.");
          pcm_frame_free (frame);
          g_object_unref (decoder);
          return FALSE;
        }
        g_message ("[live-test] writing decoded PCM to %s for %s.",
                   spotifygtk_output_backend_name (output->kind), label);
      } else {
        g_message ("[live-test] %s is decode-only; audio output will begin after full collection.",
                   label);
      }
    }

    if (output) {
      /* No volume control here: this is the decode-verification path, which
       * has no engine control to read it from. Playback goes through
       * audio_output_thread(). */
      gsize written = spotifygtk_output_write (output, frame->samples, frame->n_frames);
      if (written != frame->n_frames) {
        g_warning ("[live-test] audio output wrote %" G_GSIZE_FORMAT
                   " of %" G_GSIZE_FORMAT " PCM frames.", written, frame->n_frames);
        pcm_frame_free (frame);
        spotifygtk_output_close (output);
        g_object_unref (decoder);
        return FALSE;
      }
      written_frames += written;
    }
    total_frames += frame->n_frames;
    packets++;
    pcm_frame_free (frame);
  }

  if (packets == 0) {
    g_warning ("[live-test] decoder produced no PCM from the initial %u-byte probe.",
               CDN_DECODE_PROBE_LENGTH);
    g_object_unref (decoder);
    return FALSE;
  }

  if (output) {
    spotifygtk_output_drain (output);
    spotifygtk_output_close (output);
  }
  g_message ("[live-test] %s decoder produced %" G_GUINT64_FORMAT
             " PCM frames across %u packet(s)%s%" G_GUINT64_FORMAT " frame(s) to the audio backend.",
             label, total_frames, packets, output ? "; wrote " : "; did not write ", written_frames);
  g_object_unref (decoder);
  return TRUE;
}

static void start_read_ahead (LiveTestState *state);

static void
disable_startup_watchdog (LiveTestState *state)
{
  if (!state->timeout_source)
    return;
  g_source_destroy (state->timeout_source);
  g_source_unref (state->timeout_source);
  state->timeout_source = NULL;
  g_message ("[live-test] initial PCM output is active; network startup watchdog disabled");
}

static void
mark_output_failed (LiveTestState *state)
{
  g_mutex_lock (&state->queue_lock);
  state->output_failed = TRUE;
  g_cond_broadcast (&state->queue_cond);
  g_mutex_unlock (&state->queue_lock);
}

static gpointer
audio_output_thread (gpointer user_data)
{
  LiveTestState *state = user_data;
  SpotifyAudioOutput *output = NULL;
  SpotifyResampler   *resampler = NULL;
  gint                device_rate = 0;   /* rate the device was opened at */

  for (;;) {
    StreamQueueItem *item = g_async_queue_pop (state->pcm_queue);
    if (item->end) {
      g_free (item);
      break;
    }

    PcmFrame *frame = item->frame;
    g_free (item);

    g_mutex_lock (&state->queue_lock);
    state->queued_frames -= frame->n_frames;
    gboolean failed = state->output_failed;
    g_cond_broadcast (&state->queue_cond);
    g_mutex_unlock (&state->queue_lock);

    if (failed) {
      pcm_frame_free (frame);
      continue;
    }

    if (!output) {
      /* A target rate from the settings engages the resampler; 0 follows the
       * stream, which stays a straight copy to the device. */
      gint target = engine_control_get_output_rate (state->control);
      device_rate = (target > 0) ? target : frame->sample_rate;

      if (device_rate != frame->sample_rate) {
        resampler = spotifygtk_resampler_new (frame->channels);
        spotifygtk_resampler_set_rates (resampler, frame->sample_rate, device_rate);
        g_message ("[live-test] resampling %d Hz -> %d Hz",
                   frame->sample_rate, device_rate);
      }

      state->device_rate = device_rate;
      output = spotifygtk_output_open (device_rate, frame->channels);
      if (!output) {
        g_warning ("[live-test] streaming decoder opened, but no PCM output backend is available");
        mark_output_failed (state);
        pcm_frame_free (frame);
        continue;
      }
      g_message ("[live-test] streaming decoder opened Ogg/Vorbis: %d Hz, %d channel(s); output=%s",
                 frame->sample_rate, frame->channels,
                 spotifygtk_output_backend_name (output->kind));
      disable_startup_watchdog (state);
      report_progress (state, SPOTIFYGTK_ENGINE_PLAYING,
                       "Audio started; decoding incoming CDN ranges.");
    }

    if (!engine_control_wait (state->control, state->cancellable)) {
      g_message ("[live-test] output worker stopping current PCM frame due to cancellation");
      mark_output_failed (state);
      pcm_frame_free (frame);
      continue;
    }

    engine_control_apply_volume (state->control, frame->samples,
                                 frame->n_frames, frame->channels);
    engine_control_apply_eq (state->control, frame->samples,
                             frame->n_frames, frame->channels, frame->sample_rate);
    /* Position is reported in device frames, so it must use the rate the
     * device is running at, not the stream's. */
    gsize expected = frame->n_frames;
    gint16 *pcm = frame->samples;
    g_autofree gint16 *converted = NULL;

    if (resampler && !spotifygtk_resampler_is_passthrough (resampler)) {
      expected = spotifygtk_resampler_process (resampler, frame->samples,
                                               frame->n_frames, &converted);
      pcm = converted;
      if (expected == 0) {          /* block too short to yield a frame yet */
        pcm_frame_free (frame);
        continue;
      }
    }

    gsize written = spotifygtk_output_write (output, pcm, expected);
    if (written != expected) {
      g_warning ("[live-test] streaming audio output wrote %" G_GSIZE_FORMAT
                 " of %" G_GSIZE_FORMAT " PCM frames",
                 written, expected);
      mark_output_failed (state);
    } else {
      state->output_frames += written;
      /* Report position as it advances, so the UI progress bar can track it.
       * output_frames counts only frames actually written to the device, so a
       * paused or stalled stream holds its position rather than running on. */
      spotifygtk_native_engine_control_report_position (state->control,
                                                        state->output_frames,
                                                        device_rate);
    }
    pcm_frame_free (frame);
  }

  if (output) {
    spotifygtk_output_drain (output);
    spotifygtk_output_close (output);
  }
  spotifygtk_resampler_free (resampler);
  return NULL;
}

static void
stream_playback_start (LiveTestState *state)
{
  state->pcm_queue = g_async_queue_new ();
  g_mutex_init (&state->queue_lock);
  g_cond_init (&state->queue_cond);
  state->queue_initialized = TRUE;
  state->output_thread = g_thread_new ("spotify-native-audio", audio_output_thread, state);
  g_message ("[live-test] audio output worker started; PCM queue limit is %u frames",
             STREAM_QUEUE_MAX_FRAMES);
}

static gboolean
stream_queue_frame (LiveTestState *state, PcmFrame *frame)
{
  /* A pending seek breaks the wait promptly and rejects the frame, so the
   * read-ahead chain returns to on_read_ahead_chunk and rebuilds at the new
   * position instead of blocking here for the buffered audio to drain. */
  g_mutex_lock (&state->queue_lock);
  while (state->queued_frames >= STREAM_QUEUE_MAX_FRAMES &&
         !state->output_failed &&
         !spotifygtk_native_engine_control_seek_pending (state->control) &&
         !(state->cancellable && g_cancellable_is_cancelled (state->cancellable))) {
    g_cond_wait_until (&state->queue_cond, &state->queue_lock,
                       g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND);
  }
  gboolean rejected = state->output_failed ||
                      spotifygtk_native_engine_control_seek_pending (state->control) ||
                      (state->cancellable && g_cancellable_is_cancelled (state->cancellable));
  if (!rejected)
    state->queued_frames += frame->n_frames;
  g_mutex_unlock (&state->queue_lock);

  if (rejected) {
    pcm_frame_free (frame);
    return FALSE;
  }

  StreamQueueItem *item = g_new0 (StreamQueueItem, 1);
  item->frame = frame;
  g_async_queue_push (state->pcm_queue, item);
  return TRUE;
}

static void
stream_playback_stop (LiveTestState *state)
{
  if (!state->output_thread)
    return;

  StreamQueueItem *item = g_new0 (StreamQueueItem, 1);
  item->end = TRUE;
  g_async_queue_push (state->pcm_queue, item);
  g_thread_join (state->output_thread);
  state->output_thread = NULL;
}

static gboolean
stream_decode_chunk (LiveTestState *state, GBytes *decrypted_chunk, gboolean final_chunk)
{
  spotifygtk_decoder_feed (state->stream_decoder, decrypted_chunk);

  guint frames_pulled = 0;
  PcmFrame *frame = NULL;
  while ((frame = spotifygtk_decoder_pull (state->stream_decoder)) != NULL) {
    /* Eat the post-seek shortfall before anything reaches the device. */
    if (state->seek_discard_frames > 0) {
      if (frame->n_frames <= state->seek_discard_frames) {
        state->seek_discard_frames -= frame->n_frames;
        pcm_frame_free (frame);
        continue;
      }
      gsize drop = (gsize) state->seek_discard_frames;
      gsize keep = frame->n_frames - drop;
      memmove (frame->samples, frame->samples + drop * (gsize) frame->channels,
               keep * (gsize) frame->channels * sizeof (gint16));
      frame->n_frames = keep;
      state->seek_discard_frames = 0;
    }

    gsize frame_count = frame->n_frames;
    if (!stream_queue_frame (state, frame))
      return FALSE;
    state->stream_frames += frame_count;
    frames_pulled++;
  }

  g_message ("[live-test] streaming range decoded %u PCM frame batches (%" G_GUINT64_FORMAT
             " total frames)", frames_pulled, state->stream_frames);

  if (final_chunk) {
    stream_playback_stop (state);
    if (state->output_failed || state->output_frames == 0) {
      g_warning ("[live-test] final streaming range produced no PCM output");
      return FALSE;
    }
    g_message ("[live-test] streaming decode complete: %" G_GUINT64_FORMAT
               " decoded, %" G_GUINT64_FORMAT " written PCM frames",
               state->stream_frames, state->output_frames);
  }
  return TRUE;
}

/* ── Seek: Ogg page walking + header replay ──────────────────────────────── */

/* Parse the Ogg page at buf[pos]. Fills *page_len (total page length) and
 * *granulepos, returning TRUE only for a complete, well-formed page. */
static gboolean
ogg_page_at (const guint8 *buf, gsize len, gsize pos,
             gsize *page_len, gint64 *granulepos)
{
  if (pos + 27 > len) return FALSE;
  if (memcmp (buf + pos, "OggS", 4) != 0) return FALSE;
  if (buf[pos + 4] != 0) return FALSE;               /* structure version 0 */

  guint8 n_segments = buf[pos + 26];
  gsize  header_len = 27 + (gsize) n_segments;
  if (pos + header_len > len) return FALSE;

  gsize body = 0;
  for (guint8 i = 0; i < n_segments; i++)
    body += buf[pos + 27 + i];

  gsize total = header_len + body;
  if (pos + total > len) return FALSE;

  if (granulepos) {
    guint64 g = 0;
    for (int i = 0; i < 8; i++)
      g |= (guint64) buf[pos + 6 + i] << (8 * i);    /* little-endian */
    *granulepos = (gint64) g;
  }
  if (page_len)
    *page_len = total;
  return TRUE;
}

/* Length of the leading run of header pages (granulepos 0 or -1) — the Vorbis
 * id/comment/setup headers, which carry no audio. This prefix is replayed into
 * a fresh decoder on seek so it can decode from a mid-file page. */
static gsize
ogg_header_prefix_len (const guint8 *buf, gsize len)
{
  gsize pos = 0;
  while (pos < len) {
    gsize   page_len;
    gint64  granule;
    if (!ogg_page_at (buf, len, pos, &page_len, &granule))
      break;
    if (granule != 0 && granule != -1)
      break;                       /* first audio page: stop before it */
    pos += page_len;
  }
  return pos;
}

/* First audio page (granulepos > 0) at or after `from`, with its granulepos.
 * The scan is byte-wise for the "OggS" capture pattern, so it resynchronises
 * even though the fetched window began at an arbitrary byte offset. */
static gboolean
ogg_first_audio_page (const guint8 *buf, gsize len, gsize from,
                      gsize *out_off, gint64 *out_granule)
{
  for (gsize pos = from; pos + 4 <= len; pos++) {
    if (buf[pos] != 'O' || memcmp (buf + pos, "OggS", 4) != 0)
      continue;
    gsize   page_len;
    gint64  granule;
    if (!ogg_page_at (buf, len, pos, &page_len, &granule))
      continue;
    if (granule > 0) {
      if (out_off)     *out_off = pos;
      if (out_granule) *out_granule = granule;
      return TRUE;
    }
    pos += page_len - 1;           /* skip the page body; loop's ++ adds one */
  }
  return FALSE;
}

typedef struct {
  LiveTestState *state;
  guint          generation;
  gint64         target_ms;
  goffset        request_offset;
  gboolean       refined;      /* TRUE once the estimate has been re-aimed */
} SeekLanding;

static void do_seek (LiveTestState *state);

/* Re-arm the audio output worker after a seek teardown, reusing the existing
 * queue and lock (only the thread and the failed/counter state are reset). */
static void
stream_output_restart (LiveTestState *state)
{
  g_mutex_lock (&state->queue_lock);
  state->output_failed = FALSE;
  state->queued_frames = 0;
  g_mutex_unlock (&state->queue_lock);
  state->output_thread = g_thread_new ("spotify-native-audio", audio_output_thread, state);
}

/* The seek window arrived: find a page, replay headers into a fresh decoder,
 * feed from that page, and resume normal read-ahead from just past the window. */
static void
on_seek_landing_chunk (GBytes *decrypted_chunk, goffset offset, GError *error, gpointer user_data)
{
  SeekLanding  *land   = user_data;
  (void) offset;   /* single outstanding request; nothing to disambiguate */
  LiveTestState *state = land->state;
  guint    gen        = land->generation;
  gint64   target_ms  = land->target_ms;
  goffset  req_off    = land->request_offset;
  gboolean refined    = land->refined;
  g_free (land);

  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  /* A newer seek superseded this one: its teardown already happened, so drop
   * this stale window rather than feeding it into the wrong decoder. */
  if (gen != state->playback_generation)
    return;

  if (!decrypted_chunk) {
    g_warning ("[seek] landing fetch failed: %s", error ? error->message : "unknown error");
    state->ok = TRUE;                       /* treat as end-of-track */
    g_main_loop_quit (state->loop);
    return;
  }

  gsize         clen  = 0;
  const guint8 *cdata = g_bytes_get_data (decrypted_chunk, &clen);

  gsize  page_off = 0;
  gint64 granule  = 0;
  if (!ogg_first_audio_page (cdata, clen, 0, &page_off, &granule)) {
    g_warning ("[seek] no Ogg audio page in the landing window; treating as end");
    state->ok = TRUE;
    g_main_loop_quit (state->loop);
    return;
  }

  gint    rate          = state->stream_sample_rate > 0 ? state->stream_sample_rate : 44100;
  guint64 landed_frames = (guint64) granule;

  /* Close the remaining gap by discarding decoded frames rather than reporting
   * the landing point as the new position. Position is preset to the target so
   * the progress bar reads correctly while those frames are being eaten. */
  gint64 target_frames = target_ms * (gint64) rate / 1000;
  gint64 shortfall     = target_frames - (gint64) landed_frames;

  g_message ("[seek] target %.1fs, landed %.1fs, shortfall %.1fs%s",
             (double) target_ms / 1000.0, (double) landed_frames / rate,
             (double) shortfall / rate, refined ? " (refined)" : "");

  if (shortfall < 0)
    shortfall = 0;                       /* overshot: nothing to trim */

  /* A large shortfall means nominal bitrate lied -- which it does on VBR. We
   * now know one true (byte, sample) pair, so re-aim with the measured ratio
   * rather than discarding many seconds of decoded audio. Once only: the
   * second attempt trims whatever is left, so a bad ratio cannot loop. */
  if (!refined && landed_frames > 0 && req_off > 0 &&
      shortfall > (gint64) rate * SEEK_REFINE_SEC) {
    gdouble bytes_per_frame = (gdouble) req_off / (gdouble) landed_frames;
    gint64  aim_frames      = target_frames - (gint64) (rate * SEEK_UNDERSHOOT_MS / 1000);
    if (aim_frames < 0)
      aim_frames = 0;
    goffset new_off = (goffset) (bytes_per_frame * (gdouble) aim_frames);

    g_message ("[seek] re-aiming: %.1f bytes/frame measured, byte %" G_GOFFSET_FORMAT
               " -> %" G_GOFFSET_FORMAT, bytes_per_frame, req_off, new_off);

    SeekLanding *again    = g_new0 (SeekLanding, 1);
    again->state          = state;
    again->generation     = state->playback_generation;
    again->target_ms      = target_ms;
    again->request_offset = new_off;
    again->refined        = TRUE;

    spotifygtk_cdn_fetch_chunk (state->cdn_fetcher, state->cdn_url, state->audio_key,
                                new_off, CDN_FULL_DOWNLOAD_CHUNK,
                                on_seek_landing_chunk, again);
    return;
  }

  if (shortfall > (gint64) rate * SEEK_MAX_TRIM_SEC)
    shortfall = (gint64) rate * SEEK_MAX_TRIM_SEC;

  state->seek_discard_frames = (guint64) shortfall;

  /* output_frames is counted in DEVICE frames -- the audio worker adds what it
   * writes to the device, and position is reported against the device rate. A
   * granule is in STREAM frames. Those are the same number only when no
   * resampling is happening; with the resampler engaged (44.1k -> 48k here)
   * presetting a granule directly made every seek report a position short by
   * the rate ratio, which is 8.1% -- about 20 s at the four-minute mark. */
  gint    dev_rate      = state->device_rate > 0 ? state->device_rate : rate;
  guint64 stream_frames = landed_frames + (guint64) shortfall;
  state->output_frames  = (guint64) ((gdouble) stream_frames
                                     * (gdouble) dev_rate / (gdouble) rate);

  spotifygtk_native_engine_control_report_position (state->control,
                                                    state->output_frames, dev_rate);

  g_message ("[seek] landed at logical byte %" G_GOFFSET_FORMAT "+%" G_GSIZE_FORMAT
             ", granule %" G_GINT64_FORMAT " (%.1fs; target %.1fs)",
             req_off, page_off, granule, (double) granule / rate,
             (double) target_ms / 1000.0);

  /* Replay the cached headers so the fresh decoder can open, then feed audio
   * from the landing page onward. Feeding headers yields no PCM. */
  GBytes *hdr = g_bytes_new_static (state->header_prefix, state->header_prefix_len);
  gboolean ok = stream_decode_chunk (state, hdr, FALSE);
  g_bytes_unref (hdr);

  if (ok) {
    GBytes *tail = g_bytes_new (cdata + page_off, clen - page_off);
    ok = stream_decode_chunk (state, tail, FALSE);
    g_bytes_unref (tail);
  }

  if (!ok) {
    /* A seek requested during the tail decode is not a failure — honour it. */
    if (spotifygtk_native_engine_control_seek_pending (state->control)) {
      do_seek (state);
      return;
    }
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  state->next_download_offset = req_off + (goffset) clen;
  start_read_ahead (state);
}

/*
 * Perform a seek: tear down the current decoder and output, estimate a byte
 * offset from the nominal bitrate, and fetch a window there. The landing
 * callback finishes the job. Runs on the engine context.
 */
static void
do_seek (LiveTestState *state)
{
  gint64 target_ms = 0;
  spotifygtk_native_engine_control_take_seek (state->control, &target_ms);

  if (!state->header_prefix || state->header_prefix_len == 0) {
    g_warning ("[seek] no cached Ogg headers yet; ignoring seek");
    start_read_ahead (state);           /* keep playing from where we were */
    return;
  }

  g_message ("[seek] request: %" G_GINT64_FORMAT " ms", target_ms);

  /* Preserve pause across the seek: resuming is only to unpark the output
   * worker so it can observe the teardown (it waits on the pause condition,
   * which mark_output_failed does not signal). Re-paused after the rebuild. */
  gboolean was_paused = spotifygtk_native_engine_control_is_paused (state->control);

  spotifygtk_native_engine_control_resume (state->control);
  mark_output_failed (state);
  stream_playback_stop (state);

  g_clear_object (&state->stream_decoder);
  state->stream_decoder = spotifygtk_decoder_new ();
  state->stream_frames = 0;

  stream_output_restart (state);

  if (was_paused)
    spotifygtk_native_engine_control_pause (state->control);

  /* Deliberately aim SHORT of the target.
   *
   * The byte offset is only an estimate: nominal bitrate is not actual bitrate
   * on a VBR stream, and we then snap to the first Ogg page in the window,
   * which is another jump forward. Landing wherever that lands and calling it
   * done is what made a drag to 3:02 arrive at 2:54 -- and, because the
   * estimate is a pure function of the target, land in exactly the same wrong
   * place however many times it was retried.
   *
   * So bias the estimate back by SEEK_UNDERSHOOT_MS and let the landing
   * callback discard decoded frames forward to the exact sample. Undershooting
   * is recoverable by decoding a little extra; overshooting is not recoverable
   * without another round trip. */
  glong   bitrate = state->bitrate_nominal > 0 ? state->bitrate_nominal : 160000;
  gint64  aim_ms  = target_ms - SEEK_UNDERSHOOT_MS;
  if (aim_ms < 0)
    aim_ms = 0;
  goffset offset  = (goffset) ((gdouble) aim_ms / 1000.0 * (gdouble) bitrate / 8.0);
  if (offset < 0)
    offset = 0;

  state->seek_discard_frames = 0;

  /* Invalidate any read-ahead fetch already in flight for the old position. */
  state->playback_generation++;

  SeekLanding *land   = g_new0 (SeekLanding, 1);
  land->state         = state;
  land->generation    = state->playback_generation;
  land->target_ms     = target_ms;
  land->request_offset = offset;

  g_message ("[seek] fetching landing window at logical byte %" G_GOFFSET_FORMAT
             " (bitrate %ld bps)", offset, bitrate);
  spotifygtk_cdn_fetch_chunk (state->cdn_fetcher, state->cdn_url, state->audio_key,
                              offset, CDN_FULL_DOWNLOAD_CHUNK,
                              on_seek_landing_chunk, land);
}

#define READ_AHEAD_MAX_RETRIES 2
static void on_read_ahead_restorage (SpclientCdnUrls *urls, GError *error, gpointer user_data);

static void
on_read_ahead_chunk (GBytes *decrypted_chunk, goffset offset, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  /*
   * Only one read-ahead range is outstanding at a time, so the offset being
   * awaited is next_download_offset -- it advances only when bytes land.
   * Anything reporting a different offset is a request this run issued and
   * has since moved past: a fetch that stalled, was retried at the same
   * offset by the path below, and then had its own 45s deadline expire long
   * afterwards. Its failure used to be attributed to whatever range was in
   * flight at the time, which retried a perfectly healthy fetch and decoded
   * the same bytes twice -- audible as a few seconds of music repeating. A
   * late success is no better: it would splice bytes from the wrong part of
   * the file into the stream. Neither is ours to act on.
   */
  if (offset != state->next_download_offset) {
    g_message ("[live-test] ignoring a %s for logical offset %" G_GOFFSET_FORMAT
               "; the read-ahead has moved on to %" G_GOFFSET_FORMAT ".",
               decrypted_chunk ? "late range" : "stale failure",
               offset, state->next_download_offset);
    return;
  }

  if (!decrypted_chunk) {
    /*
     * One refused range used to end the track outright. That is the wrong
     * response to the most common cause: a CDN URL that aged out while
     * playback was paused. The bytes are still there and the file is still
     * licensed -- only the signed URL has expired -- so re-resolving and
     * retrying recovers, where giving up loses the rest of the track and
     * reads to the listener as "the audio randomly died".
     *
     * Bounded, and the counter resets on every chunk that lands, so this
     * survives a stale URL or a transient blip without spinning forever on a
     * genuinely dead stream.
     */
    if (state->read_ahead_retries < READ_AHEAD_MAX_RETRIES) {
      state->read_ahead_retries++;
      g_warning ("[live-test] read-ahead fetch failed at logical offset %" G_GOFFSET_FORMAT
                 ": %s -- re-resolving the CDN URL and retrying.",
                 state->next_download_offset, error ? error->message : "unknown error");
      spotifygtk_spclient_get_audio_storage (state->spclient, state->file_id, 20,
                                             state->bearer_token, state->client_token,
                                             on_read_ahead_restorage, state);
      return;
    }

    g_warning ("[live-test] read-ahead fetch failed at logical offset %" G_GOFFSET_FORMAT
               ": %s -- gave up after %d retries.",
               state->next_download_offset, error ? error->message : "unknown error",
               READ_AHEAD_MAX_RETRIES);
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  /* Progress made: forgive earlier failures at other offsets. */
  state->read_ahead_retries = 0;

  gsize len = 0;
  g_bytes_get_data (decrypted_chunk, &len);
  state->next_download_offset += (goffset) len;
  g_message ("[live-test] read-ahead range: supplied %" G_GSIZE_FORMAT
             " bytes at logical offset %" G_GOFFSET_FORMAT ".",
             len, state->next_download_offset - (goffset) len);

  if (!stream_decode_chunk (state, decrypted_chunk, len != CDN_FULL_DOWNLOAD_CHUNK)) {
    /* A pending seek is why stream_queue_frame rejected the frame — it is not
     * an error. Rebuild at the new position instead of ending the track. */
    if (spotifygtk_native_engine_control_seek_pending (state->control)) {
      do_seek (state);
      return;
    }
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  /* A seek can also land while a chunk decoded cleanly (queue not full). */
  if (spotifygtk_native_engine_control_seek_pending (state->control)) {
    do_seek (state);
    return;
  }

  if (len == CDN_FULL_DOWNLOAD_CHUNK) {
    start_read_ahead (state);
    return;
  }

  g_message ("[live-test] final CDN range reached; read-ahead decoder drained.");
  state->ok = TRUE;
  g_main_loop_quit (state->loop);
}

static void start_read_ahead (LiveTestState *state);

/*
 * A CDN URL went stale mid-track. Resolve fresh ones for the same file and
 * resume from where the failed range began.
 *
 * The storage response is per-file and short-lived: the URLs carry an issue
 * timestamp and the validity window is the server's, not something the client
 * can read off them. So the only way to know a URL has aged out is to have a
 * request refused, which is exactly what brings us here.
 */
static void
on_read_ahead_restorage (SpclientCdnUrls *urls, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (!urls || urls->n_urls == 0) {
    g_warning ("[live-test] could not re-resolve CDN storage after a failed "
               "range (%s); ending the track.",
               error ? error->message : "no URLs returned");
    if (urls)
      spclient_cdn_urls_free (urls);
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  g_free (state->cdn_url);
  state->cdn_url = g_strdup (urls->urls[0]);
  spclient_cdn_urls_free (urls);

  g_message ("[live-test] re-resolved CDN storage; retrying the range at offset %"
             G_GOFFSET_FORMAT " (attempt %u/%d).",
             state->next_download_offset, state->read_ahead_retries,
             READ_AHEAD_MAX_RETRIES);
  start_read_ahead (state);
}

static void
start_read_ahead (LiveTestState *state)
{
  g_message ("[live-test] requesting next full-track range at logical offset %" G_GOFFSET_FORMAT ".",
             state->next_download_offset);
  spotifygtk_cdn_fetch_chunk (state->cdn_fetcher, state->cdn_url, state->audio_key,
                               state->next_download_offset, CDN_FULL_DOWNLOAD_CHUNK,
                               on_read_ahead_chunk, state);
}

static void
on_aes_key (SpotifyApSession *session, ApCommandId cmd, const guint8 *payload, gsize len, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */
  g_message ("[live-test] received AP_CMD_AES_KEY (%" G_GSIZE_FORMAT " bytes); dispatching to audio-key client", len);
  spotifygtk_audio_key_handle_response (state->audio_key_client, payload, len);
}

static void
on_aes_key_error (SpotifyApSession *session, ApCommandId cmd, const guint8 *payload, gsize len, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */
  g_warning ("[live-test] received AP_CMD_AES_KEY_ERROR (%" G_GSIZE_FORMAT " bytes); dispatching to audio-key client", len);
  spotifygtk_audio_key_handle_error (state->audio_key_client, payload, len);
}

static void
on_cdn_seek_probe_result (GBytes *decrypted_chunk, goffset offset, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  (void) offset;   /* single outstanding request; nothing to disambiguate */
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (!decrypted_chunk) {
    g_warning ("[live-test] CDN seek-probe fetch failed: %s",
               error ? error->message : "unknown error");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  gsize initial_len = 0;
  const guint8 *initial = g_bytes_get_data (state->initial_cdn_chunk, &initial_len);
  gsize probe_len = 0;
  const guint8 *probe = g_bytes_get_data (decrypted_chunk, &probe_len);

  g_message ("[live-test] CDN seek probe decrypted %" G_GSIZE_FORMAT
             " bytes at logical offset %u.", probe_len, CDN_SEEK_PROBE_OFFSET);
  if (initial_len < CDN_SEEK_PROBE_OFFSET + probe_len) {
    g_warning ("[live-test] seek probe cannot be compared: initial range has %" G_GSIZE_FORMAT
               " bytes, needs at least %u.", initial_len,
               CDN_SEEK_PROBE_OFFSET + (guint) probe_len);
    state->ok = FALSE;
  } else if (memcmp (initial + CDN_SEEK_PROBE_OFFSET, probe, probe_len) != 0) {
    g_warning ("[live-test] FAILURE: seek probe differs from the same bytes in the initial range. "
               "CTR counter/offset handling is incorrect.");
    state->ok = FALSE;
  } else {
    g_message ("[live-test] SUCCESS: non-block-aligned CDN seek probe matches the initial decrypt.");
    state->stream_decoder = spotifygtk_decoder_new ();
    stream_playback_start (state);
    if (!stream_decode_chunk (state, state->initial_cdn_chunk, FALSE)) {
      state->ok = FALSE;
      g_main_loop_quit (state->loop);
      return;
    }

    /* Cache what a later seek needs: the Vorbis header pages (replayed into a
     * fresh decoder so it can open at a mid-file page) plus the bitrate and
     * sample rate (to estimate the seek byte offset and label the position).
     * The decoder has just opened on these bytes, so info is available now. */
    state->bitrate_nominal    = spotifygtk_decoder_get_bitrate_nominal (state->stream_decoder);
    state->stream_sample_rate = spotifygtk_decoder_get_sample_rate (state->stream_decoder);
    gsize hlen = ogg_header_prefix_len (initial, initial_len);
    if (hlen > 0 && hlen <= initial_len) {
      state->header_prefix     = g_memdup2 (initial, hlen);
      state->header_prefix_len = hlen;
      g_message ("[live-test] cached %" G_GSIZE_FORMAT " bytes of Ogg headers for seeking "
                 "(bitrate %ld bps, %d Hz)", hlen, state->bitrate_nominal,
                 state->stream_sample_rate);
    } else {
      g_warning ("[live-test] could not locate Ogg header pages; seeking will be unavailable");
    }

    g_clear_pointer (&state->initial_cdn_chunk, g_bytes_unref);
    state->next_download_offset = (goffset) initial_len;
    g_message ("[live-test] seek validation passed; starting incremental read-ahead playback.");
    start_read_ahead (state);
    return;
  }

  g_main_loop_quit (state->loop);
}

static void
on_cdn_initial_chunk_result (GBytes *decrypted_chunk, goffset offset, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  (void) offset;   /* single outstanding request; nothing to disambiguate */
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (!decrypted_chunk) {
    g_warning ("[live-test] CDN initial fetch failed: %s",
               error ? error->message : "unknown error");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (decrypted_chunk, &len);
  g_message ("[live-test] CDN initial decrypt succeeded -- %" G_GSIZE_FORMAT " bytes.", len);
  if (len < 4 || memcmp (data, "OggS", 4) != 0) {
    g_warning ("[live-test] FAILURE: initial decrypt does not start with OggS "
               "(got %02x %02x %02x %02x).", len > 0 ? data[0] : 0,
               len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  g_message ("[live-test] initial decrypt has a valid Ogg stream header; "
             "now validating a non-block-aligned seek range.");
  if (!decode_and_play (decrypted_chunk, FALSE, FALSE, "initial-decode-check")) {
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }
  state->initial_cdn_chunk = g_bytes_ref (decrypted_chunk);
  spotifygtk_cdn_fetch_chunk (state->cdn_fetcher, state->cdn_url, state->audio_key,
                               CDN_SEEK_PROBE_OFFSET, CDN_SEEK_PROBE_LENGTH,
                               on_cdn_seek_probe_result, state);
}

/*
 * Backstop for an audio-key request that never gets an answer.
 *
 * A refusal is normally explicit: the AP replies AES_KEY_ERROR carrying a
 * reason code, promptly (~200ms observed). An earlier reading of this code --
 * that the AP silently ignores unentitled requests -- was wrong. That silence
 * came from the connection already being dead, because the client was not
 * answering the AP's keepalive ping; once that was fixed, denials arrived as
 * proper error packets.
 *
 * The deadline is still worth having: without one, a genuinely stuck request
 * falls through to the run watchdog and gets reported as a network fault,
 * which is a lie about a request that reached the server. It just fires far
 * less often than the comment here used to claim.
 */
#define AUDIO_KEY_ATTEMPT_TIMEOUT_S 8

/* AudioFile::Format values from metadata.proto, so a log says what a track
 * actually offers rather than a bare integer. */
static const gchar *
audio_format_label (guint64 format)
{
  switch (format) {
    case 0:  return "OGG_VORBIS_96";
    case 1:  return "OGG_VORBIS_160";
    case 2:  return "OGG_VORBIS_320";
    case 3:  return "MP3_256";
    case 4:  return "MP3_320";
    case 8:  return "AAC_24";
    case 9:  return "AAC_48";
    case 16: return "FLAC";
    default: return "unknown";
  }
}

static void
clear_key_timeout (LiveTestState *state)
{
  if (!state->key_timeout_source)
    return;
  g_source_destroy (state->key_timeout_source);
  g_source_unref (state->key_timeout_source);
  state->key_timeout_source = NULL;
}

static gboolean
on_audio_key_attempt_timeout (gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return G_SOURCE_REMOVE;   /* the run that armed this has already finished */

  /* Drop our ref here rather than via clear_key_timeout(): returning
   * G_SOURCE_REMOVE destroys the source, and GLib holds its own ref for the
   * duration of the dispatch, so unreffing from inside the callback is safe. */
  GSource *fired = state->key_timeout_source;
  state->key_timeout_source = NULL;
  if (fired)
    g_source_unref (fired);

  g_warning ("[live-test] no audio-key response after %ds. Everything up to this "
             "point succeeded, so the request reached the server; silence here is "
             "not itself a verdict on entitlement -- an unentitled account is "
             "normally refused explicitly and promptly, with a reason code.",
             AUDIO_KEY_ATTEMPT_TIMEOUT_S);
  /* The UI only gets the generic "Native playback failed" from player_service.c;
   * there is no ERROR stage on SpotifyNativeEngineStage to carry a reason. The
   * log line above is the only place this is currently explained. */
  state->ok = FALSE;
  g_main_loop_quit (state->loop);
  return G_SOURCE_REMOVE;
}

static void
on_audio_key_result (const guint8 key[AUDIO_KEY_LEN], GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  clear_key_timeout (state);

  if (key) {
    g_message ("[live-test] AUDIO KEY SUCCEEDED -- got 16-byte AES key.");
    memcpy (state->audio_key, key, AUDIO_KEY_LEN);
    g_message ("[live-test] Fetching and decrypting initial CDN chunk "
               "(logical offset 0 -> physical offset 167)...");
    report_progress (state, SPOTIFYGTK_ENGINE_BUFFERING,
                     "Audio key received; buffering the first audio range.");
    spotifygtk_cdn_fetch_chunk (state->cdn_fetcher, state->cdn_url, state->audio_key,
                                0, CDN_DECODE_PROBE_LENGTH,
                                on_cdn_initial_chunk_result, state);
    return; /* Wait for CDN chunk */
  } else {
    /*
     * The refusal a non-Premium account gets, so say what it means rather than
     * leaving someone to guess from a bare code.
     *
     * Established against a live server on 2026-08-02: a free account is
     * refused a key for *every* file a track offers -- OGG_VORBIS 320, 160 and
     * 96 and AAC_24 alike, each within ~200ms. So the gate is the audio-key
     * exchange itself, not any format or bitrate, and trying a different file
     * does not help. librespot has required Premium for the same reason for
     * its whole existence.
     */
    g_warning ("[live-test] audio key request failed: %s. Everything before this "
               "step succeeded, so this is Spotify refusing to license the audio, "
               "not a fault in the client: the native audio-key path requires "
               "Spotify Premium, for every format a track offers.",
               error ? error->message : "unknown error");
    state->ok = FALSE;
  }

  g_main_loop_quit (state->loop);
}

static void
on_audio_storage_result (SpclientCdnUrls *urls, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (urls && urls->n_urls > 0) {
    g_message ("[live-test] STORAGE RESOLUTION SUCCEEDED -- CDN URLs:");
    for (guint i = 0; i < urls->n_urls; i++)
      g_message ("  %s", urls->urls[i]);
    
    state->cdn_url = g_strdup (urls->urls[0]);
    spclient_cdn_urls_free (urls);

    g_message ("[live-test] Requesting audio key for the track...");
    spotifygtk_audio_key_request (state->audio_key_client, state->track_gid, 16,
                                  state->file_id, 20, on_audio_key_result, state);

    /* Attached by hand for the same reason the run watchdog is: g_timeout_add()
     * binds the source to the *global default* context rather than the engine's
     * private one, where it would never fire. */
    clear_key_timeout (state);
    state->key_timeout_source = g_timeout_source_new_seconds (AUDIO_KEY_ATTEMPT_TIMEOUT_S);
    g_source_set_callback (state->key_timeout_source, on_audio_key_attempt_timeout, state, NULL);
    g_source_attach (state->key_timeout_source, state->context);
    return; /* Wait for AES key, or for that deadline */
  } else {
    g_warning ("[live-test] get_audio_storage failed: %s", error ? error->message : "unknown error");
    state->ok = FALSE;
  }

  g_main_loop_quit (state->loop);
}

static void proceed_with_credentials (LiveTestState *state);
static void cred_cache_invalidate (void);
static void wire_session_to_state (LiveTestState *state, SpotifyApSession *session);

/*
 * Objects that outlive a single track.
 *
 * The engine used to build everything per run and tear it down again, which
 * cost the whole connection setup on every track. Measured on a real start:
 * AP handshake and login ~1.9s, the audio key round trip on that fresh
 * connection ~2.1s, and the first 64KB CDN chunk ~1.0s because its session was
 * new too and paid a TCP and TLS handshake to a host it had just been talking
 * to. About 3.9 seconds of a 5.5 second start, thrown away and rebuilt each
 * time.
 *
 * The context has to persist for any of it to: the AP session's socket sources
 * belong to whichever context was thread-default when they were created, so a
 * session outliving its context would have nothing pumping it. Runs are
 * strictly sequential -- player_service only starts the next track from
 * on_engine_finished -- so exactly one thread has this pushed at a time, which
 * is what makes a shared context safe without a dedicated host thread.
 */
static GMainContext     *engine_context = NULL;
static SpotifyApSession  *engine_ap      = NULL;
static SpotifyCdnFetcher *engine_cdn     = NULL;

/*
 * Mercury rides the AP connection, so it is only valid for the session it was
 * built against. Tracked alongside so a reconnect rebuilds it rather than
 * leaving handlers registered on a session that is gone.
 */
static SpotifyMercury    *engine_mercury         = NULL;
static SpotifyApSession  *engine_mercury_session = NULL;

/*
 * Mercury probe, gated behind SPOTIFY_PROBE_MERCURY.
 *
 * hm://metadata/4/track/<gid> is used because its correct answer is already
 * known -- the same track metadata arrives over spclient -- so a 200 with a
 * plausible payload proves the framing, the Header encoding and the reply
 * matching all work. Probing an unknown endpoint first could not distinguish
 * "wrong URI" from "our packets are malformed".
 */
static void
on_mercury_probe_result (MercuryResponse *response, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;

  if (!response) {
    g_warning ("[mercury-probe] no response");
    return;
  }

  g_message ("[mercury-probe] status %d for %s, %u payload part(s)",
             response->status_code, response->uri ? response->uri : "(no uri)",
             response->parts ? response->parts->len : 0);

  for (guint i = 0; response->parts && i < response->parts->len; i++) {
    gsize len = 0;
    const guint8 *d = g_bytes_get_data (g_ptr_array_index (response->parts, i), &len);
    /* The name field is a string at field 2 of the Track message; printing it
     * is the cheapest proof the bytes are real metadata and not noise. */
    const guint8 *name = NULL; gsize name_len = 0;
    if (pb_find_bytes_field (d, len, 2, &name, &name_len)) {
      g_autofree gchar *n = g_strndup ((const gchar *) name, name_len);
      g_message ("[mercury-probe]   part %u: %" G_GSIZE_FORMAT " bytes, name=\"%s\"", i, len, n);
    } else {
      g_message ("[mercury-probe]   part %u: %" G_GSIZE_FORMAT " bytes", i, len);
    }
  }
}

static void
on_track_metadata_result (const SpclientAudioFile *files, guint n_files, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (files) {
    g_message ("[live-test] TRACK METADATA SUCCEEDED -- found %u files:", n_files);
    gint best_idx = -1;
    for (guint i = 0; i < n_files; i++) {
      g_message ("  file[%u]: format=%" G_GUINT64_FORMAT " = %s",
                 i, files[i].format, audio_format_label (files[i].format));
      
      /* OGG_VORBIS_320 = 2, OGG_VORBIS_160 = 1, OGG_VORBIS_96 = 0 */
      if (files[i].format == 2) best_idx = i;
      else if (best_idx == -1 && files[i].format == 1) best_idx = i;
      else if (best_idx == -1 && files[i].format == 0) best_idx = i;
    }
    
    if (best_idx != -1) {
      g_message ("[live-test] Selected format %" G_GUINT64_FORMAT ". Resolving CDN URLs...", files[best_idx].format);
      report_progress (state, SPOTIFYGTK_ENGINE_BUFFERING,
                       "Track metadata received; resolving audio storage.");
      
      memcpy (state->file_id, files[best_idx].file_id, 20);
      memcpy (state->track_gid, files[best_idx].track_gid, 16);

      if (g_getenv ("SPOTIFY_PROBE_MERCURY") && engine_mercury) {
        gchar hex[33];
        for (int i = 0; i < 16; i++)
          g_snprintf (hex + i * 2, 3, "%02x", state->track_gid[i]);
        g_autofree gchar *muri = g_strdup_printf ("hm://metadata/4/track/%s", hex);
        g_message ("[mercury-probe] GET %s", muri);
        spotifygtk_mercury_request (engine_mercury, MERCURY_METHOD_GET, muri,
                                    NULL, on_mercury_probe_result, state);
      }
      
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
    /* This is the first request made with the streaming credentials, so it is
     * where a stale or revoked pair shows up. Drop the cache rather than
     * re-serving it for the rest of the hour and failing every track. */
    cred_cache_invalidate ();
    state->ok = FALSE;
  }

  g_main_loop_quit (state->loop);
}

/*
 * Context-resolve probe.
 *
 * The response shape of /context-resolve/v1 for a `search` URI is not
 * documented anywhere upstream -- librespot's own comment says only that it
 * is "massively influenced by the provided query". The only way to learn it
 * is to ask a real server, which is what this does.
 *
 * Gated behind SPOTIFY_PROBE_CONTEXT so it never runs during normal
 * playback. When set, the probe replaces playback rather than preceding it:
 * interleaving a diagnostic with the audio pipeline would make it unclear
 * which one any subsequent failure came from.
 */
static void
on_batch_probe_result (const SpclientTrackInfo *tracks, guint n_tracks,
                       GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (error || !tracks) {
    g_warning ("[probe] batched metadata FAILED: %s",
               error ? error->message : "no tracks returned");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  g_message ("[probe] batched metadata SUCCEEDED -- %u track(s), one round trip", n_tracks);
  g_message ("[probe] --- what the UI would render ---");

  for (guint i = 0; i < n_tracks; i++) {
    const SpclientTrackInfo *t = &tracks[i];
    gint secs = (gint) (t->meta.duration_ms / 1000);
    g_message ("[probe]  %2u. %-42s %-28s %d:%02d%s",
               i + 1,
               t->meta.name         ? t->meta.name         : "(no name)",
               t->meta.artist_names ? t->meta.artist_names : "(no artist)",
               secs / 60, secs % 60,
               t->meta.is_explicit ? "  [E]" : "");
  }

  state->ok = TRUE;
  g_main_loop_quit (state->loop);
}

static void
on_context_probe_result (JsonNode *context, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (error || !context) {
    g_warning ("[probe] context-resolve FAILED: %s",
               error ? error->message : "no data returned");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  g_message ("[probe] context-resolve SUCCEEDED -- dumping response shape");

  /* Top-level keys, then the first page's first track, which is the part
   * the UI actually has to consume. */
  if (JSON_NODE_HOLDS_OBJECT (context)) {
    JsonObject *root = json_node_get_object (context);
    g_autoptr(GList) keys = json_object_get_members (root);
    GString *key_list = g_string_new (NULL);
    for (GList *l = keys; l; l = l->next) {
      if (key_list->len) g_string_append (key_list, ", ");
      g_string_append (key_list, (const gchar *) l->data);
    }
    g_message ("[probe] top-level keys: %s", key_list->str);
    g_string_free (key_list, TRUE);

    if (json_object_has_member (root, "pages")) {
      JsonArray *pages = json_object_get_array_member (root, "pages");
      guint n_pages = pages ? json_array_get_length (pages) : 0;
      g_message ("[probe] pages: %u", n_pages);

      if (n_pages > 0) {
        JsonObject *page = json_array_get_object_element (pages, 0);
        if (page && json_object_has_member (page, "tracks")) {
          JsonArray *tracks = json_object_get_array_member (page, "tracks");
          guint n_tracks = tracks ? json_array_get_length (tracks) : 0;
          g_message ("[probe] page[0].tracks: %u", n_tracks);

          if (n_tracks > 0) {
            g_autoptr(JsonGenerator) gen = json_generator_new ();
            json_generator_set_pretty (gen, TRUE);
            json_generator_set_root (gen, json_array_get_element (tracks, 0));
            g_autofree gchar *first = json_generator_to_data (gen, NULL);
            g_message ("[probe] page[0].tracks[0] =\n%s", first);
          }
        }
      }
    }
  }

  /* Second half: context-resolve yields URIs only, so resolve them to
   * display metadata in ONE batched call and print what the UI would
   * actually render. This is the part that proves the whole catalog path,
   * not just that the endpoint answers. */
  GPtrArray *uris = g_ptr_array_new_with_free_func (g_free);
  if (JSON_NODE_HOLDS_OBJECT (context)) {
    JsonObject *root = json_node_get_object (context);
    if (json_object_has_member (root, "pages")) {
      JsonArray *pages = json_object_get_array_member (root, "pages");
      for (guint p = 0; pages && p < json_array_get_length (pages); p++) {
        JsonObject *page = json_array_get_object_element (pages, p);
        if (!page || !json_object_has_member (page, "tracks"))
          continue;
        JsonArray *tracks = json_object_get_array_member (page, "tracks");
        for (guint t = 0; tracks && t < json_array_get_length (tracks); t++) {
          JsonObject *track = json_array_get_object_element (tracks, t);
          if (!track || !json_object_has_member (track, "uri"))
            continue;
          g_ptr_array_add (uris,
            g_strdup (json_object_get_string_member (track, "uri")));
        }
      }
    }
  }

  json_node_unref (context);

  if (uris->len == 0) {
    g_warning ("[probe] no track URIs in context -- nothing to resolve");
    g_ptr_array_free (uris, TRUE);
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  /* A real library is thousands of tracks; one request for all of them is
   * rejected. Cap the probe so we can find the workable batch size. */
  const gchar *limit_env = g_getenv ("SPOTIFY_PROBE_LIMIT");
  guint limit = limit_env && *limit_env ? (guint) g_ascii_strtoull (limit_env, NULL, 10) : 50;
  if (limit > 0 && uris->len > limit) {
    g_message ("[probe] capping %u URIs to %u for this batch", uris->len, limit);
    g_ptr_array_set_size (uris, limit);
  }

  g_message ("[probe] resolving %u track URIs in a single batched request...", uris->len);
  g_ptr_array_add (uris, NULL);   /* NULL-terminate for the const gchar *const * form */

  /* Stashed so the metadata callback can free it after the request. */
  g_object_set_data_full (G_OBJECT (state->spclient), "probe-uris", uris,
                          (GDestroyNotify) g_ptr_array_unref);

  spotifygtk_spclient_get_tracks_metadata (state->spclient,
                                           (const gchar *const *) uris->pdata,
                                           uris->len - 1,
                                           state->bearer_token, state->client_token,
                                           on_batch_probe_result, state);
}

/*
 * Streaming credentials, cached for the life of the process.
 *
 * The engine runs its whole auth chain once per *track*, and on a real run the
 * fixed part of that costs about a second before a byte of audio is fetched:
 * handshake and login ~360ms, then client-token ~280ms and login5 ~430ms. The
 * last two are plain HTTP and their results are good for an hour, so paying for
 * them again on every track was buying nothing.
 *
 * Cached with the expiry the server states, minus a margin so a token cannot
 * lapse between the check here and its use a few hundred milliseconds later.
 * The AP handshake still happens per track -- that is a bigger change and is
 * left alone.
 */
static GMutex  cred_cache_lock;
static gchar  *cred_cache_bearer;
static gchar  *cred_cache_client_token;
static gint64  cred_cache_expiry_us;   /* monotonic */

#define CRED_CACHE_MARGIN_US (60 * G_USEC_PER_SEC)

/* Returns TRUE and fills both out-params (newly allocated) when a usable pair
 * is cached. */
static gboolean
cred_cache_take (gchar **bearer, gchar **client_token)
{
  gboolean hit = FALSE;

  g_mutex_lock (&cred_cache_lock);
  if (cred_cache_bearer && cred_cache_client_token &&
      g_get_monotonic_time () < cred_cache_expiry_us) {
    *bearer       = g_strdup (cred_cache_bearer);
    *client_token = g_strdup (cred_cache_client_token);
    hit = TRUE;
  }
  g_mutex_unlock (&cred_cache_lock);

  return hit;
}

static void
cred_cache_store (const gchar *bearer, const gchar *client_token,
                  gint32 expires_in_seconds)
{
  if (!bearer || !client_token)
    return;

  g_mutex_lock (&cred_cache_lock);
  g_free (cred_cache_bearer);
  g_free (cred_cache_client_token);
  cred_cache_bearer       = g_strdup (bearer);
  cred_cache_client_token = g_strdup (client_token);

  gint64 life = (gint64) (expires_in_seconds > 0 ? expires_in_seconds : 3600)
                * G_USEC_PER_SEC;
  if (life > CRED_CACHE_MARGIN_US)
    life -= CRED_CACHE_MARGIN_US;
  cred_cache_expiry_us = g_get_monotonic_time () + life;
  g_mutex_unlock (&cred_cache_lock);
}

/* Dropped when a request built on these is refused, so a revoked or otherwise
 * bad pair cannot be re-served for the rest of the hour. */
static void
cred_cache_invalidate (void)
{
  g_mutex_lock (&cred_cache_lock);
  g_clear_pointer (&cred_cache_bearer, g_free);
  g_clear_pointer (&cred_cache_client_token, g_free);
  cred_cache_expiry_us = 0;
  g_mutex_unlock (&cred_cache_lock);
}

static void
on_login5_result (const gchar *access_token, gint32 expires_in_seconds,
                  GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (access_token) {
    g_message ("[live-test] LOGIN5 SUCCEEDED -- obtained a spclient-usable bearer token "
              "(expires in %ds).", expires_in_seconds);

    state->bearer_token = g_strdup (access_token);
    cred_cache_store (access_token, state->client_token, expires_in_seconds);
    proceed_with_credentials (state);
    return;
  }
  else {
    g_warning ("[live-test] login5 failed: %s", error ? error->message : "unknown error");
    g_warning ("[live-test] AP login itself succeeded -- only the streaming auth-relay "
              "chain (client-token/login5) failed. See spotify/login5.h and spotify/"
              "clienttoken.h.");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }
}

/*
 * Everything after the streaming credentials are in hand, whether they were
 * fetched or came from the cache. Split out so a cache hit re-enters at exactly
 * the same point rather than duplicating this.
 */
static void
proceed_with_credentials (LiveTestState *state)
{
  {
    state->spclient = spotifygtk_spclient_new ();
    spotifygtk_spclient_set_cancellable (state->spclient, state->cancellable);

    const gchar *probe = g_getenv ("SPOTIFY_PROBE_CONTEXT");
    if (probe && *probe) {
      /* A bare word is treated as a search query; anything already in
       * spotify:...:... form is passed through, so the same switch can
       * probe collection/playlist/album URIs too. */
      g_autofree gchar *probe_uri = NULL;
      if (g_str_has_prefix (probe, "spotify:")) {
        probe_uri = g_strdup (probe);
      } else if (g_strcmp0 (probe, "collection") == 0) {
        probe_uri = spotifygtk_spclient_build_collection_uri (
                      spotifygtk_ap_session_get_username (state->session));
      } else {
        probe_uri = spotifygtk_spclient_build_search_uri (probe);
      }

      g_message ("[probe] resolving context: %s", probe_uri ? probe_uri : "(null)");
      spotifygtk_spclient_get_context (state->spclient, probe_uri,
                                       state->bearer_token, state->client_token,
                                       on_context_probe_result, state);
      return;
    }

    g_message ("[live-test] Fetching metadata for %s...", state->track_uri);
    report_progress (state, SPOTIFYGTK_ENGINE_BUFFERING,
                     "Streaming token received; fetching track metadata.");
    spotifygtk_spclient_get_track_metadata (state->spclient, state->track_uri,
                                            state->bearer_token, state->client_token,
                                            on_track_metadata_result, state);
    return; /* Wait for track metadata */
  }
}

static void
on_client_token_result (const gchar *token, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (token) {
    g_message ("[live-test] client-token obtained, proceeding to login5...");
    report_progress (state, SPOTIFYGTK_ENGINE_BUFFERING,
                     "Client token received; authorizing the streaming session.");
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
  spotifygtk_login5_set_cancellable (state->login5_client, state->cancellable);
  spotifygtk_login5_auth_token (state->login5_client,
                                NATIVE_AUTH_CLIENT_ID, state->device_id,
                                username, creds, creds_len, creds_type,
                                token, on_login5_result, state);
}

/*
 * Attach this run's state to an AP session, new or reused.
 *
 * Every line here must run per track even when the connection is reused,
 * because all of it points at the current LiveTestState. The audio-key
 * handlers are the sharp edge: they carry `state` as user_data, so leaving a
 * previous track's registration in place would hand the next key reply a
 * pointer to a stack frame that no longer exists. Re-registering overwrites
 * it, which is why this is a function rather than something done once at
 * connect time.
 */
static void
wire_session_to_state (LiveTestState *state, SpotifyApSession *session)
{
  state->session = session;
  state->audio_key_client = spotifygtk_audio_key_client_new (session);

  /* Reused across tracks so its connections stay warm. A fresh fetcher opened
   * a new TLS connection for the first chunk of every track, which measured
   * ~1s for 64KB against ~10ms once a connection exists. */
  if (engine_mercury_session != session) {
    g_clear_object (&engine_mercury);
    engine_mercury = spotifygtk_mercury_new (session);
    engine_mercury_session = session;
  }

  if (!engine_cdn)
    engine_cdn = spotifygtk_cdn_fetcher_new ();
  state->cdn_fetcher = engine_cdn;
  spotifygtk_cdn_fetcher_set_cancellable (state->cdn_fetcher, state->cancellable);

  spotifygtk_ap_session_set_handler (session, AP_CMD_AES_KEY, on_aes_key, state);
  spotifygtk_ap_session_set_handler (session, AP_CMD_AES_KEY_ERROR, on_aes_key_error, state);
}

/* Replies arrive out of order, so the method has to travel with the request
 * or a 400 among 405s cannot be attributed to the verb that earned it. */
typedef struct { LiveTestState *state; gchar *method; } WriteProbe;

static void
on_collection_write_probe (MercuryResponse *response, gpointer user_data)
{
  WriteProbe *wp = user_data;
  if (!run_is_current (wp->state) || !response) { g_free (wp); return; }

  const gchar *verdict = "";
  if (response->status_code == 200)      verdict = "   *** ACCEPTED ***";
  else if (response->status_code == 400) verdict = "   <- method OK, payload rejected";

  g_message ("[collection-probe] %-6s -> status %d  %s%s",
             wp->method, response->status_code,
             response->uri ? response->uri : "(no uri)", verdict);

  /* Raw bytes: a 200 that decodes to nothing is either an encoding fault on
   * the way out or a parse fault on the way back, and only the wire tells
   * which. */
  for (guint i = 0; response->parts && i < response->parts->len; i++) {
    gsize len = 0;
    const guint8 *d = g_bytes_get_data (g_ptr_array_index (response->parts, i), &len);
    g_autoptr(GString) hex = g_string_new (NULL);
    for (gsize b = 0; b < len && b < 96; b++)
      g_string_append_printf (hex, "%02x", d[b]);
    g_message ("[collection-probe]   part %u: %" G_GSIZE_FORMAT " bytes  %s%s",
               i, len, hex->str, len > 96 ? "..." : "");
    const gchar *dump = g_getenv ("SPOTIFY_PROBE_DUMP_DIR");
    if (dump) {
      g_autofree gchar *fn = g_strdup_printf ("%s/part-%s-%u.bin", dump, wp->method, i);
      g_file_set_contents (fn, (const gchar *) d, (gssize) len, NULL);
      g_message ("[collection-probe]   wrote %s", fn);
    }
  }
  if (response->parts && response->parts->len == 0)
    g_message ("[collection-probe]   (no payload parts at all)");
  g_free (wp->method);
  g_free (wp);
}

/* Reads a page of the collection. Safe -- no mutation -- and doubles as the
 * membership check that makes a like/unlike round trip non-destructive: a
 * track already in the set must not be removed on the way out. */
static void
on_collection_read_probe (gboolean ok, guint16 status,
                          SpotifyCollectionItem *items, guint n_items,
                          const gchar *next_page_token, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;

  g_message ("[collection-read] status %u (%s), %u item(s), next=%s",
             status, ok ? "ok" : "failed", n_items,
             next_page_token ? next_page_token : "(none)");

  const gchar *probe_uri = g_getenv ("SPOTIFY_PROBE_COLLECTION_MEMBER");
  for (guint i = 0; i < n_items && i < 5; i++)
    g_message ("[collection-read]   %s  added_at=%d%s", items[i].uri,
               items[i].added_at, items[i].is_removed ? " (removed)" : "");

  if (probe_uri) {
    gboolean found = FALSE;
    for (guint i = 0; i < n_items; i++)
      if (g_strcmp0 (items[i].uri, probe_uri) == 0 && !items[i].is_removed) { found = TRUE; break; }
    g_message ("[collection-read] MEMBERSHIP: %s is %sin this page", probe_uri,
               found ? "" : "NOT ");
  }
}

static void
on_login_result (gboolean success, const gchar *username, GError *error, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return;   /* the run that asked for this has already finished */

  if (!success) {
    g_warning ("[live-test] login failed: %s", error ? error->message : "unknown error");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  /*
   * Subscription probe. Spotify pushes some events unprompted -- the pusher
   * connection id arrives with no subscribe at all -- so the goal here is to
   * find which URI carries collection changes when a track is liked on another
   * device. A SUB that is accepted says the URI names a real service; the
   * event that follows says what it publishes, which is the part no schema in
   * any public tree records.
   */
  /*
   * Write-endpoint probe. Sends a WriteRequest carrying *no items*, which is
   * a well-formed request that cannot change the library whatever it hits.
   * The status alone answers the only open question -- whether this URI
   * accepts a collection write -- without betting a real like on a guess.
   */
  const gchar *read_probe = g_getenv ("SPOTIFY_PROBE_COLLECTION_READ");
  if (read_probe && *read_probe && engine_mercury && username) {
    g_autofree gchar *rep = strstr (read_probe, "%s")
      ? g_strdup_printf (read_probe, username) : g_strdup (read_probe);
    g_message ("[collection-read] PageRequest -> %s", rep);
    spotifygtk_collection_read_page (engine_mercury, rep, username,
                                     SPOTIFYGTK_COLLECTION_SET_LIKED,
                                     NULL, 200, on_collection_read_probe, state);
  }

  const gchar *write_probe = g_getenv ("SPOTIFY_PROBE_COLLECTION_WRITE");
  if (write_probe && *write_probe && engine_mercury && username) {
    /*
     * Sweeps the Header method string as well as the URI, because 405 says
     * the URI was found and the verb was not accepted -- so holding the verb
     * fixed at "SEND" while varying only the path cannot converge.
     */
    const gchar *mlist = g_getenv ("SPOTIFY_PROBE_COLLECTION_METHODS");
    g_auto(GStrv) mv = g_strsplit (mlist && *mlist ? mlist : "SEND,POST,PUT,MODIFY", ",", -1);
    const gchar **methods = (const gchar **) mv;
    g_auto(GStrv) paths = g_strsplit (write_probe, ",", -1);

    /* Which body to send: the write request by default, or a PageRequest when
     * chasing why a read returns 200 with nothing in it. */
    const gchar *body_kind = g_getenv ("SPOTIFY_PROBE_COLLECTION_BODY");
    const gchar *turi      = g_getenv ("SPOTIFY_PROBE_COLLECTION_URI");
    const gchar *removeenv = g_getenv ("SPOTIFY_PROBE_COLLECTION_REMOVE");
    gboolean     removing  = removeenv && *removeenv;
    g_autoptr(GByteArray) body = NULL;

    if (g_strcmp0 (body_kind, "page") == 0) {
      body = spotifygtk_collection_build_page_request (username,
                                                       SPOTIFYGTK_COLLECTION_SET_LIKED,
                                                       NULL, 200);
    } else if (g_strcmp0 (body_kind, "write-gid") == 0 && turi) {
      /*
       * The item shape the *response* uses -- type/id/added_at at 1/2/5 with
       * a raw GID -- rather than collection2v2's uri-string CollectionItem.
       * Worth trying because the response proved that schema is not what this
       * service speaks, even though its PageRequest is accepted.
       */
      const gchar *b62 = strrchr (turi, ':');
      b62 = b62 ? b62 + 1 : turi;
      guint8 gid[16] = { 0 };
      for (int i = 0; i < 16; i++) gid[i] = 0;
      /* base62 -> 16-byte big-endian, by repeated multiply-add. */
      for (const gchar *c = b62; *c; c++) {
        const gchar *digits = "0123456789abcdefghijklmnopqrstuvwxyz"
                              "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        const gchar *at = strchr (digits, *c);
        if (!at) continue;
        guint carry = (guint) (at - digits);
        for (int i = 15; i >= 0; i--) {
          guint v = (guint) gid[i] * 62 + carry;
          gid[i] = (guint8) (v & 0xff);
          carry  = v >> 8;
        }
      }
      g_autoptr(GByteArray) item = g_byte_array_new ();
      pb_write_bytes_field (item, 2, gid, sizeof (gid));
      pb_write_varint_field (item, 5, (guint64) time (NULL));
      if (removing) {
        const gchar *fenv = g_getenv ("SPOTIFY_PROBE_REMOVE_FIELD");
        guint32 rf = fenv && *fenv ? (guint32) atoi (fenv) : 3;
        pb_write_varint_field (item, rf, 1);   /* candidate is_removed slot */
        g_message ("[collection-probe] is_removed candidate = field %u", rf);
      }

      body = g_byte_array_new ();
      if (g_getenv ("SPOTIFY_PROBE_COLLECTION_BARE")) {
        /* Symmetric with the response, which is a bare `repeated Item = 1`
         * with no username/set envelope -- the URI already names the user. */
        pb_write_message_field (body, 1, item->data, item->len);
      } else {
        pb_write_bytes_field (body, 1, (const guint8 *) username, strlen (username));
        pb_write_bytes_field (body, 2, (const guint8 *) SPOTIFYGTK_COLLECTION_SET_LIKED,
                              strlen (SPOTIFYGTK_COLLECTION_SET_LIKED));
        pb_write_message_field (body, 3, item->data, item->len);
      }
    } else {
      const gchar *uris[] = { turi, NULL };
      body = spotifygtk_collection_build_write (username, SPOTIFYGTK_COLLECTION_SET_LIKED,
                                                turi ? uris : NULL, turi ? 1 : 0,
                                                (gint32) time (NULL), removing);
    }
    g_message ("[collection-probe] body=%s (%u bytes)",
               body_kind ? body_kind : "write", body->len);
    for (guint i = 0; paths[i]; i++) {
      g_autofree gchar *wep = strstr (paths[i], "%s")
        ? g_strdup_printf (paths[i], username) : g_strdup (paths[i]);
      for (guint m = 0; methods[m]; m++) {
        g_autoptr(GBytes) payload = g_bytes_new (body->data, body->len);
        WriteProbe *wp = g_new0 (WriteProbe, 1);
        /* Copied: the method list is a g_auto(GStrv) freed when this scope
         * exits, long before an async reply lands on it. */
        wp->state = state; wp->method = g_strdup (methods[m]);
        spotifygtk_mercury_request_full (engine_mercury, MERCURY_METHOD_SEND,
                                         methods[m], wep, payload,
                                         on_collection_write_probe, wp);
      }
    }
  }

  const gchar *sub_probe = g_getenv ("SPOTIFY_PROBE_MERCURY_SUB");
  if (sub_probe && *sub_probe && engine_mercury) {
    const gchar *user = username ? username : "";
    g_auto(GStrv) uris = g_strsplit (sub_probe, ",", -1);
    for (guint i = 0; uris[i]; i++) {
      g_autofree gchar *expanded = NULL;
      /* "%s" in a candidate is the username slot. Anything else printf would
       * interpret is a caller error, but this is a developer-only probe. */
      if (strstr (uris[i], "%s"))
        expanded = g_strdup_printf (uris[i], user);
      g_message ("[mercury-sub] subscribing to %s", expanded ? expanded : uris[i]);
      spotifygtk_mercury_subscribe (engine_mercury, expanded ? expanded : uris[i],
                                    on_mercury_probe_result, state);
    }
  }

  g_message ("[live-test] LOGIN SUCCEEDED%s%s -- handshake, crypto, and login all verified "
            "against a real Spotify server.", username ? " as " : "", username ? username : "");

  /* Chain into the streaming auth-relay: client-token, then login5,
   * using the reusable credential APWelcome handed back (NOT the
   * original native_auth OAuth token -- see spotify/login5.h). This
   * is the next real unknown worth proving against actual Spotify
   * services rather than just building and hoping. */
  if (cred_cache_take (&state->bearer_token, &state->client_token)) {
    g_message ("[live-test] reusing cached streaming credentials -- skipping "
               "client-token and login5 (~700ms saved on this track).");
    report_progress (state, SPOTIFYGTK_ENGINE_BUFFERING,
                     "Connected to Spotify; preparing the streaming session.");
    proceed_with_credentials (state);
    return;
  }

  g_message ("[live-test] AP login confirmed -- now testing the streaming auth-relay chain "
            "(client-token -> login5)...");
  report_progress (state, SPOTIFYGTK_ENGINE_BUFFERING,
                   "Connected to Spotify; preparing the streaming session.");
  state->client_token_client = spotifygtk_client_token_new ();
  spotifygtk_client_token_set_cancellable (state->client_token_client, state->cancellable);
  spotifygtk_client_token_request (state->client_token_client, NATIVE_AUTH_CLIENT_ID,
                                   state->device_id,
                                   on_client_token_result, state);
}

/* Enough to ride out a short refusal burst without masking a real outage. */
#define AP_CONNECT_MAX_ATTEMPTS 4

static void on_connected (GObject *source, GAsyncResult *result, gpointer user_data);

static gboolean
retry_ap_connect (gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return G_SOURCE_REMOVE;   /* the run that armed this has already finished */
  SpotifyApSession *session = state->pending_session;

  if (!session) {
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return G_SOURCE_REMOVE;
  }

  spotifygtk_ap_session_connect (session, NULL, on_connected, state);
  return G_SOURCE_REMOVE;
}

static void
on_connected (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyApSession *session = SPOTIFYGTK_AP_SESSION (source);
  LiveTestState     *state  = user_data;
  g_autoptr(GError)  err    = NULL;

  if (!spotifygtk_ap_session_connect_finish (session, result, &err)) {
    /* Spotify refuses new AP connections once a client opens them too
     * quickly, and this engine opens a fresh one for every track — so
     * "Connection refused" here is routine after a few switches rather than
     * a genuine outage. Retry a few times with a rising delay before giving
     * up; the real fix is to stop logging in per track at all (see
     * AP_CONNECT_MAX_ATTEMPTS below and the note in run_live_test). */
    if (state->connect_attempts < AP_CONNECT_MAX_ATTEMPTS &&
        !(state->cancellable && g_cancellable_is_cancelled (state->cancellable))) {
      state->connect_attempts++;
      guint delay_ms = 400u * state->connect_attempts;
      g_warning ("[live-test] handshake failed (%s); retry %u/%u in %ums",
                 err ? err->message : "unknown error",
                 state->connect_attempts, AP_CONNECT_MAX_ATTEMPTS, delay_ms);

      /* NOT g_timeout_add(): that attaches to the *global default* context, so
       * the retry fires on the GTK main thread. Everything chained off the
       * reconnect — client-token, login5, spclient, and every CDN range fetch —
       * then binds to the main context too, because libsoup/GTask capture the
       * thread-default at call time. Two real consequences, both seen in core
       * dumps: on_range_response ran on the UI thread, where stream_queue_frame
       * blocks in g_cond_wait_until and freezes the window; and the engine state
       * machine ran on two threads at once, which freed a socket client out from
       * under the other thread (SIGSEGV in g_object_ref). Attach to the engine's
       * own context so the retry stays on the worker, like every other source
       * here. */
      GSource *retry = g_timeout_source_new (delay_ms);
      g_source_set_callback (retry, retry_ap_connect, state, NULL);
      g_source_attach (retry, state->context);
      g_source_unref (retry);
      return;
    }

    g_warning ("[live-test] handshake failed: %s", err ? err->message : "unknown error");
    state->ok = FALSE;
    g_main_loop_quit (state->loop);
    return;
  }

  g_message ("[live-test] handshake succeeded -- DH exchange, RSA signature verification, "
            "and HMAC key derivation all checked out against a real server.");
  report_progress (state, SPOTIFYGTK_ENGINE_BUFFERING,
                   "Secure AP channel established; signing in.");

  wire_session_to_state (state, session);
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
  if (!run_is_current (state))
    return G_SOURCE_REMOVE;   /* the run that armed this has already finished */
  g_warning ("[live-test] timed out after 25s waiting for a response -- "
             "cancelling all in-flight AP/auth/CDN operations; check network reachability "
             "to ap.spotify.com, or a firewall/proxy issue");
  state->timed_out = TRUE;
  if (state->cancellable)
    g_cancellable_cancel (state->cancellable);
  g_main_loop_quit (state->loop);
  return G_SOURCE_REMOVE;
}

/* The UI cancelled (Stop, or a switch to another track). Quit the engine loop
 * so the worker unwinds. Without this the cancellable was only *checked* at the
 * pause/queue waits and threaded into the CDN fetcher -- so a stream that had
 * stalled with no async op in flight (nothing to error out and quit the loop)
 * left the loop running forever, the task never completed, and every later
 * track was unplayable until the client was restarted. Runs on the engine
 * context, so quitting the loop here is safe. */
static gboolean
on_engine_cancelled (GCancellable *cancellable, gpointer user_data)
{
  LiveTestState *state = user_data;
  if (!run_is_current (state))
    return G_SOURCE_REMOVE;   /* the run that armed this has already finished */
  g_message ("[live-test] engine cancelled by the UI -- quitting the loop to unwind");
  g_main_loop_quit (state->loop);
  (void) cancellable;
  return G_SOURCE_CONTINUE;
}

static gboolean
run_live_test (const gchar *token, GCancellable *cancellable,
               SpotifyNativeEngineProgressFunc progress,
               gpointer progress_data,
               SpotifyNativeEngineControl *control,
               const gchar *track_uri)
{
  g_message ("=== live AP connection test ===");
  g_message ("Attempting a real handshake + login against Spotify's actual AP "
            "service. This is the test that proves (or disproves) interop, "
            "which nothing offline can confirm.");

  /* The engine runs in a worker owned by the GTK service. Keep all AP,
   * libsoup, and timeout sources on a private context so they cannot acquire
   * GTK's default context and dispatch protocol callbacks on the UI thread. */
  if (!engine_context)
    engine_context = g_main_context_new ();
  GMainContext *context = engine_context;
  g_main_context_push_thread_default (context);

  /* Reuse the connection when it is still up and logged in. is_live() checks
   * both, plus that a socket exists -- connected alone would let a session
   * whose login failed, or one mid-handshake, through, and neither can serve
   * an audio key. */
  gboolean reusing_ap = engine_ap && spotifygtk_ap_session_is_live (engine_ap);
  if (!reusing_ap) {
    g_clear_object (&engine_ap);
    engine_ap = spotifygtk_ap_session_new ();
  }
  SpotifyApSession *session = engine_ap;
  /* Stashed here rather than threaded through as a separate callback
   * parameter -- on_connected() needs it and is reached via
   * spotifygtk_ap_session_connect()'s fixed GAsyncReadyCallback
   * signature, which only carries `session` and our own user_data
   * (already used for `state`). g_object_set_data() is the simplest
   * way to attach a second piece of data to the same object without
   * inventing a wrapper struct just for this. */
  g_object_set_data_full (G_OBJECT (session), "login-token", g_strdup (token), g_free);

  GMainLoop *loop = g_main_loop_new (context, FALSE);
  LiveTestState state = {
    .loop = loop, .ok = FALSE, .timed_out = FALSE,
    .device_id = generate_device_id (),
    .session = NULL, .client_token_client = NULL, .login5_client = NULL,
    .spclient = NULL, .audio_key_client = NULL, .cdn_fetcher = NULL,
    .bearer_token = NULL, .client_token = NULL, .cdn_url = NULL,
    .track_uri = track_uri,
    .initial_cdn_chunk = NULL,
    .next_download_offset = 0,
    .header_prefix = NULL, .header_prefix_len = 0, .playback_generation = 0,
    .stream_decoder = NULL, .stream_frames = 0, .output_frames = 0,
    .pcm_queue = NULL, .output_thread = NULL, .queued_frames = 0,
    .output_failed = FALSE, .queue_initialized = FALSE,
    .timeout_source = NULL,
    .key_timeout_source = NULL,
    .context = context,
    .control = control,
    .progress = progress, .progress_data = progress_data,
    .cancellable = cancellable,
  };
  /* Claim the engine for this run. Callbacks compare against this before
   * touching their state pointer, so anything left over from a previous track
   * is dropped instead of writing into a stack frame that has returned. */
  engine_active_state = &state;

  report_progress (&state, SPOTIFYGTK_ENGINE_CONNECTING,
                   "Starting the native Spotify playback engine.");
  /*
   * The session gets a cancellable of its own, not this run's.
   *
   * Handing it the per-run cancellable is what stopped the connection ever
   * being reused: ending a track cancels that, ap.c treats the cancellation as
   * a dropped connection and disconnects, so the next track always found a
   * dead session and reconnected. The probe showed it immediately -- track two
   * logged a fresh handshake.
   *
   * Stopping a track still works: the engine loop is quit by its own cancel
   * source, and the run's own clients (audio key, spclient, CDN) are cancelled
   * with it. What no longer happens is tearing down a connection the next
   * track wants.
   */
  if (!reusing_ap) {
    GCancellable *ap_life = g_cancellable_new ();
    spotifygtk_ap_session_set_cancellable (session, ap_life);
    g_object_set_data_full (G_OBJECT (session), "ap-lifetime-cancellable",
                            ap_life, g_object_unref);
  }
  g_message ("[live-test] device_id for this run: %s", state.device_id);

  /* retry_ap_connect() needs the session, and on_connected's user_data slot
   * is already taken by `state`. Kept on the state itself: an earlier version
   * stashed it on the GMainLoop, which is not a GObject, so g_object_set_data
   * asserted and the retry could never find it. */
  state.pending_session = session;

  if (reusing_ap) {
    /*
     * The expensive half skipped entirely: no apresolve, no TCP, no
     * Diffie-Hellman, no login. Wire this run's state onto the live session
     * and pick up where a fresh connection would have arrived anyway --
     * on_login_result's continuation, which takes the cached credentials and
     * goes straight to metadata.
     *
     * start_receiving is not called again: the receive loop from the original
     * connection is still running, and starting a second would have two
     * readers pulling from one Shannon stream, each advancing recv_nonce past
     * the other's packets.
     */
    g_message ("[live-test] reusing the live AP connection -- skipping handshake "
               "and login (~1.9s saved on this track).");
    wire_session_to_state (&state, session);
    report_progress (&state, SPOTIFYGTK_ENGINE_BUFFERING,
                     "Connected to Spotify; preparing the streaming session.");

    if (cred_cache_take (&state.bearer_token, &state.client_token)) {
      proceed_with_credentials (&state);
    } else {
      /* Credentials expired while the connection stayed up. The AP is still
       * good; only the HTTP tokens need re-minting. */
      state.client_token_client = spotifygtk_client_token_new ();
      spotifygtk_client_token_set_cancellable (state.client_token_client, cancellable);
      spotifygtk_client_token_request (state.client_token_client, NATIVE_AUTH_CLIENT_ID,
                                       state.device_id, on_client_token_result, &state);
    }
  } else {
    spotifygtk_ap_session_connect (session, NULL, on_connected, &state);
  }

  /* Bound how long we'll wait -- a hang here (e.g. SRV resolution
   * stalling, or a firewalled outbound connection) should fail
   * loudly rather than block forever. This is now mostly a true
   * network-hang detector: a clean rejection (bad token, etc.) fails
   * fast via ap.c's "disconnected" signal rather than waiting out
   * this timeout, so reaching it specifically suggests a stuck
   * connection, not a normal login rejection. Set to 25s
   * now that a successful AP login chains into two more real network
   * round trips (client-token, login5) before the test concludes.
   *
   * Cancellation is shared with every async hop. The timeout cancels before
   * leaving the loop, so a GUI-owned engine task can safely unwind instead of
   * leaving network callbacks attached to a stack-local LiveTestState. */
  state.timeout_source = g_timeout_source_new_seconds (25);
  g_source_set_callback (state.timeout_source, on_live_test_timeout, &state, NULL);
  g_source_attach (state.timeout_source, context);

  /* Unlike the startup watchdog above (disabled once audio flows), this lives
   * for the whole run: a Stop or track switch mid-stream must quit the loop
   * even when no async hop is in flight to carry the cancellation. */
  if (cancellable) {
    state.cancel_source = g_cancellable_source_new (cancellable);
    g_source_set_callback (state.cancel_source,
                           G_SOURCE_FUNC (on_engine_cancelled), &state, NULL);
    g_source_attach (state.cancel_source, context);
  }

  g_main_loop_run (loop);

  /* on_live_test_timeout() returns G_SOURCE_REMOVE, which destroys the
   * source when it fires. For a normal success/failure exit, destroy the
   * source directly: g_source_remove() only searches the default context,
   * while this source belongs to the private engine context. */
  if (state.timeout_source) {
    if (!state.timed_out)
      g_source_destroy (state.timeout_source);
    g_source_unref (state.timeout_source);
    state.timeout_source = NULL;
  }

  /* Outlives the loop if playback started before it fired, or if the loop was
   * quit by cancellation while a key request was still in flight. */
  clear_key_timeout (&state);

  if (state.cancel_source) {
    g_source_destroy (state.cancel_source);
    g_source_unref (state.cancel_source);
    state.cancel_source = NULL;
  }

  /*
   * Drain before the stack-backed state goes away. This runs on *every* exit,
   * not only cancellation, and it waits rather than just sweeping what is
   * already queued.
   *
   * Both of those changed because the context now outlives the run. It used to
   * be created and destroyed per track, so a pending libsoup callback died
   * with it; now it persists, and a response arriving after the run returned
   * lands on a LiveTestState that is a dead stack frame. That produced a
   * SIGSEGV in SPOTIFYGTK_IS_AUDIO_KEY_CLIENT, reached from
   * on_audio_storage_result -- a storage resolve that completed successfully
   * after its run had ended.
   *
   * The old drain could not have caught it. It only ran when cancelled or
   * timed out, and g_main_context_pending() is false while a request is
   * genuinely in flight -- nothing is *ready* until the response arrives -- so
   * it exited immediately having waited for nothing.
   *
   * Cancelling first is what makes the wait short: a cancelled soup operation
   * completes in milliseconds, so this costs nothing on a normal track switch
   * while still closing the window.
   */
  /* Released before the drain, not after: anything still in flight is by
   * definition no longer this run's concern, and clearing it first means a
   * callback that lands mid-drain takes the early return rather than acting on
   * a state that is about to go away. */
  engine_active_state = NULL;

  /*
   * The caller's cancellable is deliberately NOT cancelled here.
   *
   * Doing that broke auto-advance. player_service decides what a finished run
   * meant by asking g_cancellable_is_cancelled(), so cancelling on the way out
   * made every successful track report CANCELLED -- "Playback stopped" --
   * which the UI reads as the user having stopped, so it never started the
   * next track. A clean end of stream looked identical to pressing stop.
   *
   * Cancelling was only ever belt-and-braces for late callbacks, and
   * run_is_current() already covers those properly: anything still in flight
   * finds the run gone and returns before touching it. Leaving those requests
   * to finish on their own wastes a little work and costs nothing else.
   */

  {
    gint64 deadline = g_get_monotonic_time () + 400 * G_TIME_SPAN_MILLISECOND;
    guint  drained  = 0;
    guint  idle_passes = 0;

    while (g_get_monotonic_time () < deadline) {
      if (g_main_context_iteration (context, FALSE)) {
        drained++;
        idle_passes = 0;
        continue;
      }
      /* Nothing dispatched. Give in-flight cancellations a moment to land,
       * and stop once several passes in a row find nothing at all. */
      if (++idle_passes > 20)
        break;
      g_usleep (1000);
    }
    g_message ("[live-test] drain processed %u pending context iteration(s)",
               drained);
  }
  g_main_loop_unref (loop);
  g_free (state.device_id);
  g_clear_object (&state.client_token_client);
  g_clear_object (&state.login5_client);
  g_clear_object (&state.spclient);
  g_clear_object (&state.audio_key_client);
  /* Borrowed from engine_cdn and shared across tracks -- unreffing it here is
   * what left the next run with a freed fetcher and a failed type assertion.
   * Just drop this run's pointer to it. */
  state.cdn_fetcher = NULL;
  g_free (state.bearer_token);
  g_free (state.client_token);
  g_free (state.cdn_url);
  g_clear_pointer (&state.initial_cdn_chunk, g_bytes_unref);
  g_clear_pointer (&state.header_prefix, g_free);
  stream_playback_stop (&state);
  if (state.pcm_queue)
    g_async_queue_unref (state.pcm_queue);
  if (state.queue_initialized) {
    g_mutex_clear (&state.queue_lock);
    g_cond_clear (&state.queue_cond);
  }
  g_clear_object (&state.stream_decoder);

  /*
   * The session, the CDN fetcher and the context deliberately survive this
   * function -- that is the whole point. What must not survive is a session
   * that has been cancelled or dropped: cancelling the engine tears down the
   * AP connection, and a dead session handed to the next track would fail
   * every request on it. is_live() is false by then because disconnect()
   * clears both flags, so the next run rebuilds rather than reusing.
   *
   * The handlers registered on it still point at this stack frame, which is
   * safe only because wire_session_to_state() overwrites them on the next
   * track before anything can arrive for it.
   */
  g_main_context_pop_thread_default (context);

  return state.ok;
}

/* ── Headless seek test (SPOTIFY_TEST_SEEK) ──────────────────────────────── */

typedef struct {
  SpotifyNativeEngineControl *control;   /* borrowed */
  gchar                      *spec;      /* owned: "ms[,ms...]" */
} SeekTest;

/* Fire the requested seeks a few seconds apart so playback has started and, on
 * the second seek, has run past the first landing point. Purely diagnostic. */
static gpointer
seek_test_thread (gpointer user_data)
{
  SeekTest *st = user_data;
  g_auto(GStrv) parts = g_strsplit (st->spec, ",", -1);

  /* Let auth + first-chunk decode complete before the first seek. */
  g_usleep (7 * G_USEC_PER_SEC);

  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i])
      continue;
    gint64 ms = g_ascii_strtoll (parts[i], NULL, 10);
    g_message ("[seek-test] requesting seek to %" G_GINT64_FORMAT " ms", ms);
    spotifygtk_native_engine_control_request_seek (st->control, ms);
    g_usleep (5 * G_USEC_PER_SEC);   /* let it land and play before the next */
  }

  g_free (st->spec);
  g_free (st);
  return NULL;
}

gboolean
spotifygtk_native_engine_run (GCancellable *cancellable,
                              SpotifyNativeEngineProgressFunc progress,
                              gpointer progress_data,
                              SpotifyNativeEngineControl *control,
                              const gchar *track_uri)
{
  g_message ("=== spotify-native engine harness (%s build) ===", APP_PROFILE);
  g_message ("    PipeWire: %s", HAVE_PIPEWIRE ? "yes" : "no");
  g_message ("    PulseAudio: %s", HAVE_PULSE ? "yes" : "no");
  g_message ("    ALSA: %s", HAVE_ALSA ? "yes" : "no");
  g_message ("    OpenSSL (CDN decrypt): %s", HAVE_OPENSSL ? "yes" : "no");
  g_message ("Native playback pipeline: AP auth -> CDN decrypt -> Ogg/Vorbis -> local PCM.");

  gboolean shannon_ok = run_shannon_selftest ();
  if (!shannon_ok) {
    g_warning ("Shannon self-test FAILED -- see messages above");
    return FALSE;
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
    return FALSE;
  }

  const gchar *selected_track = (track_uri && *track_uri) ? track_uri :
                                g_getenv ("SPOTIFY_TRACK_URI");
  if (!selected_track || !*selected_track)
    selected_track = DEFAULT_TEST_TRACK_URI;
  g_message ("[engine] selected track URI: %s", selected_track);

  /* SPOTIFY_TEST_SEEK="<ms>[,<ms>...]" exercises the seek path from the
   * headless harness: with no UI to drive it, a background thread fires the
   * given seeks a few seconds apart against a fresh control, so the page-scan
   * + header-replay path can be confirmed against live CDN data from logs.
   * Off by default; the normal run passes the caller's control through. */
  SpotifyNativeEngineControl *run_control = control;
  SpotifyNativeEngineControl *test_control = NULL;
  GThread *seek_thread = NULL;
  const gchar *seek_env = g_getenv ("SPOTIFY_TEST_SEEK");
  if (seek_env && *seek_env && !control) {
    test_control = spotifygtk_native_engine_control_new ();
    /* Silent: the full pipeline (decode, seek, device writes) still runs, so
     * logs confirm the seek path, but nothing is audible. */
    spotifygtk_native_engine_control_set_volume (test_control, 0.0);
    run_control  = test_control;
    SeekTest *st = g_new0 (SeekTest, 1);
    st->control = test_control;
    st->spec    = g_strdup (seek_env);
    seek_thread = g_thread_new ("seek-test", seek_test_thread, st);
  }

  gboolean live_ok = run_live_test (token, cancellable, progress, progress_data,
                                    run_control, selected_track);

  if (seek_thread) {
    /* The test thread holds a borrowed control pointer and has finished firing
     * by the time playback ends; join before freeing the control. */
    g_thread_join (seek_thread);
    spotifygtk_native_engine_control_free (test_control);
  }
  return live_ok;
}

#ifndef SPOTIFYGTK_ENGINE_LIBRARY

/* ── Session probe ───────────────────────────────────────────────────────── */

typedef struct {
  GMainLoop *loop;
  gboolean   ok;
  gchar     *query;
} SessionProbe;

static void
on_session_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  SessionProbe         *probe   = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(GPtrArray) tracks =
    spotifygtk_native_session_load_tracks_finish (session, result, &err);

  if (!tracks) {
    g_warning ("[session-probe] load_tracks FAILED: %s", err ? err->message : "unknown");
    probe->ok = FALSE;
    g_main_loop_quit (probe->loop);
    return;
  }

  g_message ("[session-probe] loaded %u track(s) via the session API", tracks->len);
  for (guint i = 0; i < tracks->len; i++) {
    SpotifyNativeTrack *t = g_ptr_array_index (tracks, i);
    gint secs = (gint) (t->duration_ms / 1000);
    g_message ("[session-probe]  %2u. %-40s %-26s %d:%02d  [%s | %s]",
               i + 1,
               t->name    ? t->name    : "(no name)",
               t->artists ? t->artists : "(no artist)",
               secs / 60, secs % 60,
               t->album_uri  ? t->album_uri  : "no-album-uri",
               t->artist_uri ? t->artist_uri : "no-artist-uri");
  }

  probe->ok = tracks->len > 0;
  g_main_loop_quit (probe->loop);
}

static void
on_session_state_changed (SpotifyNativeSession *session, gint state,
                          const gchar *message, gpointer user_data)
{
  SessionProbe *probe = user_data;

  g_message ("[session-probe] state=%d: %s", state, message ? message : "");

  if (state == SPOTIFYGTK_SESSION_READY) {
    g_autofree gchar *username = spotifygtk_native_session_dup_username (session);
    g_message ("[session-probe] session READY (username resolved: %s)",
               username ? "yes" : "no");

    g_autofree gchar *uri = NULL;
    if (g_strcmp0 (probe->query, "collection") == 0)
      uri = spotifygtk_native_session_dup_collection_uri (session);
    else
      uri = spotifygtk_spclient_build_search_uri (probe->query);

    g_message ("[session-probe] load_tracks(%s)", uri ? uri : "(null)");
    spotifygtk_native_session_load_tracks (session, uri, 10, NULL,
                                           on_session_tracks_loaded, probe);
  } else if (state == SPOTIFYGTK_SESSION_FAILED) {
    probe->ok = FALSE;
    g_main_loop_quit (probe->loop);
  }
}

/*
 * Exercises SpotifyNativeSession the way the UI will: create it on the main
 * thread, start it, wait for "state-changed", then call the async API and
 * confirm the result arrives back on this thread. That last part is the
 * whole point -- the protocol work happens on the session's worker, and a
 * callback landing on the wrong thread is exactly the bug this design is
 * meant to prevent.
 */
/*
 * Disposes a session while it is still signing in, which is the race the
 * loop-publishing lock in session.c exists for: if dispose() sees a NULL
 * loop and the worker then starts one, g_thread_join blocks forever. A
 * SIGTERM cannot test this -- the process dies without running dispose --
 * so it has to be driven explicitly. Prints and exits, or hangs on failure.
 */
static int
run_session_dispose_probe (void)
{
  for (int i = 0; i < 5; i++) {
    SpotifyNativeSession *session = spotifygtk_native_session_new ();
    spotifygtk_native_session_start (session);

    /* Stagger across the window between "thread spawned" and "loop running",
     * so at least one iteration lands inside it. */
    g_usleep ((gulong) i * 40 * 1000);

    g_message ("[dispose-probe] disposing after %d ms...", i * 40);
    g_object_unref (session);
    g_message ("[dispose-probe] iteration %d returned cleanly", i);
  }

  g_message ("[dispose-probe] PASSED -- no deadlock");
  return 0;
}

static int
run_session_probe (const gchar *query)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  SessionProbe probe = { .loop = loop, .ok = FALSE, .query = g_strdup (query) };

  SpotifyNativeSession *session = spotifygtk_native_session_new ();
  g_signal_connect (session, "state-changed",
                    G_CALLBACK (on_session_state_changed), &probe);

  g_message ("[session-probe] starting session (main thread: %p)", (void *) g_thread_self ());
  spotifygtk_native_session_start (session);

  g_main_loop_run (loop);

  g_message ("[session-probe] %s", probe.ok ? "PASSED" : "FAILED");

  g_object_unref (session);
  g_main_loop_unref (loop);
  g_free (probe.query);
  return probe.ok ? 0 : 1;
}

int
main (int argc, char *argv[])
{
  (void) argc; (void) argv;

  const gchar *session_probe = g_getenv ("SPOTIFY_PROBE_SESSION");
  if (session_probe && *session_probe) {
    if (g_strcmp0 (session_probe, "dispose") == 0)
      return run_session_dispose_probe ();
    return run_session_probe (session_probe);
  }

  return spotifygtk_native_engine_run (NULL, NULL, NULL, NULL, NULL) ? 0 : 1;
}
#endif

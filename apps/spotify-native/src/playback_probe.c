/*
 * playback_probe.c — Reproduces "after a few tracks nothing plays until the
 * app is restarted".
 *
 * The report is that playback works for a while, then every subsequent track
 * silently does nothing. player_service is the prime suspect: it gates every
 * start on `self->task` being NULL, so if one engine run never finishes, the
 * task is never cleared and each later request just replaces `pending_uri`
 * and returns TRUE. From the UI that is indistinguishable from a dead click.
 *
 * That cannot be tested from the harness — player_service is only linked into
 * the GUI binary — and it cannot be tested by hand without playing audio out
 * loud, so this drives it directly: sign in, take a handful of real track
 * URIs, and start them back to back at volume 0, recording which state each
 * one reaches.
 *
 * Run with:
 *   SPOTIFY_PROBE_PLAYBACK=6 ./build/src/spotify-native
 *
 * Reads as a table of iterations. A run that reaches PLAYING every time did
 * not reproduce; a run that stops reaching PLAYING at iteration N has, and N
 * is the interesting number — if it matches the GTask thread-pool size, the
 * cause is engine runs not terminating rather than anything in the UI.
 */

#include "config.h"
#include "playback_probe.h"

#include "player_service.h"
#include "spotify/session.h"

/* Long enough for a cold AP handshake plus CDN buffering; short enough that a
 * wedged iteration is obvious rather than looking like a slow one. */
#define STATE_TIMEOUT_SECONDS   35
/* Let a track actually play before switching, so each iteration exercises the
 * "switch while playing" path rather than "switch while still connecting". */
#define PLAY_DWELL_SECONDS       6

typedef struct {
  GMainLoop *loop;
  SpotifyNativeSession *session;
  SpotifyNativePlayerService *player;

  GPtrArray *tracks;        /* SpotifyNativeTrack*, owned */
  guint      wanted;        /* how many iterations to attempt */
  guint      index;         /* current iteration, 0-based */

  gboolean   reached_playing;
  guint      timeout_id;
  guint      dwell_id;
  guint      failed_at;     /* first iteration that never reached PLAYING */
} PlaybackProbe;

static void start_next_track (PlaybackProbe *probe);

static const gchar *
state_name (gint state)
{
  switch (state) {
    case SPOTIFYGTK_PLAYER_IDLE:       return "IDLE";
    case SPOTIFYGTK_PLAYER_CONNECTING: return "CONNECTING";
    case SPOTIFYGTK_PLAYER_BUFFERING:  return "BUFFERING";
    case SPOTIFYGTK_PLAYER_PLAYING:    return "PLAYING";
    case SPOTIFYGTK_PLAYER_PAUSED:     return "PAUSED";
    case SPOTIFYGTK_PLAYER_STOPPING:   return "STOPPING";
    case SPOTIFYGTK_PLAYER_ERROR:      return "ERROR";
    default:                           return "?";
  }
}

static void
finish (PlaybackProbe *probe)
{
  g_clear_handle_id (&probe->timeout_id, g_source_remove);
  g_clear_handle_id (&probe->dwell_id, g_source_remove);

  g_message ("[playback-probe] ─────────────────────────────────");
  if (probe->failed_at == 0) {
    g_message ("[playback-probe] PASSED — %u/%u iterations reached PLAYING; "
               "the wedge did not reproduce", probe->wanted, probe->wanted);
  } else {
    g_message ("[playback-probe] REPRODUCED — iteration %u never reached "
               "PLAYING (iterations 1..%u did)",
               probe->failed_at, probe->failed_at - 1);
    g_message ("[playback-probe] player_service state is now %s; "
               "is_active=%s",
               state_name (spotifygtk_player_service_get_state (probe->player)),
               spotifygtk_player_service_is_active (probe->player) ? "yes" : "no");
    g_message ("[playback-probe] if is_active is still yes, an engine run "
               "never finished and every later start was queued behind it");
  }

  g_main_loop_quit (probe->loop);
}

/* The track had its turn; move on, which is the switch-while-playing path. */
static gboolean
on_dwell_elapsed (gpointer user_data)
{
  PlaybackProbe *probe = user_data;
  probe->dwell_id = 0;
  start_next_track (probe);
  return G_SOURCE_REMOVE;
}

static gboolean
on_state_timeout (gpointer user_data)
{
  PlaybackProbe *probe = user_data;
  probe->timeout_id = 0;

  g_warning ("[playback-probe]  %2u. TIMED OUT after %ds — never reached PLAYING",
             probe->index + 1, STATE_TIMEOUT_SECONDS);

  if (probe->failed_at == 0)
    probe->failed_at = probe->index + 1;

  finish (probe);
  return G_SOURCE_REMOVE;
}

static void
on_player_state (SpotifyNativePlayerService *player, gint state,
                 const gchar *message, gpointer user_data)
{
  PlaybackProbe *probe = user_data;

  g_message ("[playback-probe]      state=%s (%s)", state_name (state),
             message ? message : "");

  if (state == SPOTIFYGTK_PLAYER_PLAYING && !probe->reached_playing) {
    probe->reached_playing = TRUE;
    g_clear_handle_id (&probe->timeout_id, g_source_remove);
    g_message ("[playback-probe]  %2u. OK — reached PLAYING", probe->index + 1);

    /* Let it run briefly, then switch. */
    probe->dwell_id = g_timeout_add_seconds (PLAY_DWELL_SECONDS,
                                             on_dwell_elapsed, probe);
  } else if (state == SPOTIFYGTK_PLAYER_ERROR) {
    g_warning ("[playback-probe]  %2u. ERROR — %s", probe->index + 1,
               message ? message : "unknown");
    if (probe->failed_at == 0)
      probe->failed_at = probe->index + 1;
    finish (probe);
  }
  (void) player;
}

static void
start_next_track (PlaybackProbe *probe)
{
  probe->index++;

  if (probe->index >= probe->wanted || probe->index >= probe->tracks->len) {
    finish (probe);
    return;
  }

  const SpotifyNativeTrack *track = g_ptr_array_index (probe->tracks, probe->index);
  probe->reached_playing = FALSE;

  g_message ("[playback-probe] ── iteration %u: %s", probe->index + 1,
             track->name ? track->name : track->uri);

  g_autoptr(GError) error = NULL;
  if (!spotifygtk_player_service_start_uri (probe->player, track->uri, &error)) {
    g_warning ("[playback-probe]  %2u. REFUSED — %s", probe->index + 1,
               error ? error->message : "unknown");
    if (probe->failed_at == 0)
      probe->failed_at = probe->index + 1;
    finish (probe);
    return;
  }

  probe->timeout_id = g_timeout_add_seconds (STATE_TIMEOUT_SECONDS,
                                             on_state_timeout, probe);
}

static void
on_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  PlaybackProbe *probe = user_data;
  g_autoptr(GError) error = NULL;

  GPtrArray *tracks = spotifygtk_native_session_load_tracks_finish (
    SPOTIFYGTK_NATIVE_SESSION (source), result, &error);

  if (!tracks || tracks->len == 0) {
    g_warning ("[playback-probe] could not load tracks: %s",
               error ? error->message : "empty result");
    g_main_loop_quit (probe->loop);
    return;
  }

  probe->tracks = tracks;
  g_message ("[playback-probe] loaded %u track(s); cycling %u of them at "
             "volume 0", tracks->len, MIN (probe->wanted, tracks->len));

  /* Silent: this is a diagnostic, not a listening session. */
  spotifygtk_player_service_set_volume (probe->player, 0);

  probe->index = (guint) -1;   /* start_next_track increments first */
  start_next_track (probe);
}

static void
on_session_state (SpotifyNativeSession *session, gint state,
                  const gchar *message, gpointer user_data)
{
  PlaybackProbe *probe = user_data;

  if (state == SPOTIFYGTK_SESSION_FAILED) {
    g_warning ("[playback-probe] sign-in failed: %s", message ? message : "");
    g_main_loop_quit (probe->loop);
    return;
  }
  if (state != SPOTIFYGTK_SESSION_READY)
    return;

  g_autofree gchar *uri = spotifygtk_native_session_dup_collection_uri (session);
  if (!uri) {
    g_warning ("[playback-probe] no collection URI (username unresolved)");
    g_main_loop_quit (probe->loop);
    return;
  }

  spotifygtk_native_session_load_tracks (session, uri, probe->wanted + 2, NULL,
                                         on_tracks_loaded, probe);
}

int
spotifygtk_run_playback_probe (const gchar *spec)
{
  guint wanted = spec && *spec ? (guint) g_ascii_strtoull (spec, NULL, 10) : 6;
  if (wanted == 0)
    wanted = 6;

  g_message ("[playback-probe] cycling %u tracks; each plays %ds before the "
             "next starts", wanted, PLAY_DWELL_SECONDS);

  PlaybackProbe probe = { 0 };
  probe.loop    = g_main_loop_new (NULL, FALSE);
  probe.wanted  = wanted;
  probe.player  = spotifygtk_player_service_new ();
  probe.session = spotifygtk_native_session_new ();

  g_signal_connect (probe.player, "state-changed",
                    G_CALLBACK (on_player_state), &probe);
  g_signal_connect (probe.session, "state-changed",
                    G_CALLBACK (on_session_state), &probe);

  spotifygtk_native_session_start (probe.session);
  g_main_loop_run (probe.loop);

  g_clear_handle_id (&probe.timeout_id, g_source_remove);
  g_clear_handle_id (&probe.dwell_id, g_source_remove);
  g_clear_pointer (&probe.tracks, g_ptr_array_unref);
  g_clear_object (&probe.player);
  g_clear_object (&probe.session);
  g_main_loop_unref (probe.loop);

  return probe.failed_at == 0 ? 0 : 1;
}

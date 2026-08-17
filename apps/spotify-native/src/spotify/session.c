/*
 * session.c — Long-lived authenticated Spotify session.
 *
 * The sign-in chain here is the one proven by the SPOTIFY_PROBE_CONTEXT
 * probe in main.c: AP connect → login → client-token → login5. The
 * difference is that it stops there and stays open, instead of continuing
 * into a playback pipeline and tearing itself down.
 */

#include "config.h"
#include "session.h"

#include "ap.h"
#include "clienttoken.h"
#include "login5.h"
#include "native_auth.h"
#include "spclient.h"
#include "track_meta.h"

#include "protobuf_min.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>

struct _SpotifyNativeSession {
  GObject parent_instance;

  /* Worker thread + its private context. Created in start(), torn down in
   * dispose(). Everything protocol-related runs here. */
  GThread      *thread;
  GMainContext *context;
  GMainLoop    *loop;

  /* Context the session was created on — where signals are emitted. */
  GMainContext *caller_context;

  GCancellable *cancellable;

  /* Worker-owned protocol objects. */
  SpotifyApSession   *ap;
  SpotifyMercury     *mercury;   /* lazily built on `ap` */
  SpotifyClientToken *client_token_client;
  SpotifyLogin5      *login5_client;
  SpotifySpclient    *spclient;
  gchar              *device_id;
  gchar              *login_token;
  guint               connect_attempts;

  /* Worker-thread only: the refresh handshake and the requests waiting on it. */
  gboolean  refreshing;
  GList    *pending_ops;

  /*
   * One shared copy of the collection.
   *
   * Home, Library and Liked Songs all resolve the same collection URI, and all
   * three are handed the session at sign-in -- so the same several-thousand
   * track load ran three times concurrently, each paying its own paging round
   * trips and keeping its own copy. Cached here, on the worker thread that owns
   * every load, so the second and third callers are served from memory.
   *
   * Deliberately not a general cache: only the collection URI is shared between
   * pages, and a search or an album is per-view and short-lived.
   */
  gchar     *collection_cache_uri;    /* which URI the cache holds */
  GPtrArray *collection_cache;        /* SpotifyNativeTrack*, owned */

  /*
   * Requests waiting on a collection load already in flight.
   *
   * A cache alone was not enough: all three pages are handed the session in the
   * same breath at sign-in and ask concurrently, so every one of them missed an
   * empty cache and started its own load. Measured, before this: six paging
   * requests where two would do. Coalescing is what actually removes the
   * duplication; the cache then covers later navigations.
   */
  gchar  *collection_inflight_uri;
  GList  *collection_waiters;         /* GTask*, each holding its own ref */

  /* Shared state: written by the worker, read by the main thread. */
  GMutex   lock;
  SpotifyNativeSessionState state;
  gchar   *bearer_token;
  gchar   *client_token;
  gchar   *username;
  gint64   bearer_expires_at;   /* monotonic; 0 = unknown */
  gboolean stopping;            /* dispose has begun; worker must not start its loop */
};

/* Re-mint this far ahead of expiry, so a request issued just under the wire
 * does not land after it. */
#define BEARER_REFRESH_MARGIN_US (60 * G_USEC_PER_SEC)

G_DEFINE_FINAL_TYPE (SpotifyNativeSession, spotifygtk_native_session, G_TYPE_OBJECT)

enum { STATE_CHANGED, TRANSFER_REQUESTED, REMOTE_COMMAND, N_SIGNALS };
static guint signals[N_SIGNALS];

void
spotifygtk_native_track_free (SpotifyNativeTrack *track)
{
  if (!track)
    return;
  g_free (track->uri);
  g_free (track->name);
  g_free (track->artists);
  g_free (track->album);
  g_free (track->cover_id);
  g_free (track->cover_id_small);
  g_free (track->album_uri);
  g_free (track->artist_uri);
  g_free (track);
}

SpotifyNativeTrack *
spotifygtk_native_track_copy (const SpotifyNativeTrack *track)
{
  if (!track)
    return NULL;

  SpotifyNativeTrack *copy = g_new0 (SpotifyNativeTrack, 1);
  copy->uri         = g_strdup (track->uri);
  copy->name        = g_strdup (track->name);
  copy->artists     = g_strdup (track->artists);
  copy->album       = g_strdup (track->album);
  copy->duration_ms = track->duration_ms;
  copy->is_explicit = track->is_explicit;
  copy->cover_id    = g_strdup (track->cover_id);
  copy->cover_id_small = g_strdup (track->cover_id_small);
  copy->release_year = track->release_year;
  copy->album_uri   = g_strdup (track->album_uri);
  copy->artist_uri  = g_strdup (track->artist_uri);
  return copy;
}

/*
 * A controller's instruction, on its way to the main thread.
 *
 * The dealer socket lives on the worker thread, so everything it decodes
 * arrives there. Emitting straight from that thread ran the window's handlers
 * -- and through them GTK widget code -- concurrently with the frame clock,
 * which is a data race GTK gives no protection against. It surfaced as a SEGV
 * deep inside gtk_widget_measure with no frame of ours on the stack and
 * nothing for ASan to attribute, because nothing had been freed: two threads
 * were simply in the widget tree at once.
 */
typedef struct {
  SpotifyNativeSession *session;
  gchar                *text;      /* track uri, or command endpoint */
  gint64                number;    /* position, or seek target */
  gboolean              flag;      /* paused */
  gboolean              is_command;
} RemoteEvent;

static gboolean
dispatch_remote (gpointer user_data)
{
  RemoteEvent *event = user_data;

  if (event->is_command)
    g_signal_emit (event->session, signals[REMOTE_COMMAND], 0,
                   event->text, event->number);
  else
    g_signal_emit (event->session, signals[TRANSFER_REQUESTED], 0,
                   event->text, event->number, event->flag);

  g_object_unref (event->session);
  g_free (event->text);
  g_free (event);
  return G_SOURCE_REMOVE;
}

/* Hand a decoded instruction to the thread that owns the widgets. */
static void
emit_remote (SpotifyNativeSession *self, gboolean is_command,
             const gchar *text, gint64 number, gboolean flag)
{
  if (!self || !text)
    return;

  RemoteEvent *event = g_new0 (RemoteEvent, 1);
  event->session    = g_object_ref (self);
  event->text       = g_strdup (text);
  event->number     = number;
  event->flag       = flag;
  event->is_command = is_command;
  g_main_context_invoke (self->caller_context, dispatch_remote, event);
}

/* ── State reporting ─────────────────────────────────────────────────────── */

typedef struct {
  SpotifyNativeSession     *session;
  SpotifyNativeSessionState state;
  gchar                    *message;
} StateEvent;

static gboolean
dispatch_state (gpointer user_data)
{
  StateEvent *event = user_data;
  g_signal_emit (event->session, signals[STATE_CHANGED], 0,
                 (gint) event->state, event->message);
  g_object_unref (event->session);
  g_free (event->message);
  g_free (event);
  return G_SOURCE_REMOVE;
}

/* Called on the worker thread. Updates shared state under the lock, then
 * marshals the signal to the caller's context. */
static void
set_state (SpotifyNativeSession *self, SpotifyNativeSessionState state,
           const gchar *message)
{
  g_mutex_lock (&self->lock);
  self->state = state;
  g_mutex_unlock (&self->lock);

  StateEvent *event = g_new0 (StateEvent, 1);
  event->session = g_object_ref (self);
  event->state   = state;
  event->message = g_strdup (message);
  g_main_context_invoke (self->caller_context, dispatch_state, event);
}

static void
fail_session (SpotifyNativeSession *self, const gchar *message)
{
  g_warning ("session: %s", message);
  set_state (self, SPOTIFYGTK_SESSION_FAILED, message);
}

/* ── Sign-in chain (worker thread) ───────────────────────────────────────── */

static gchar *
generate_device_id (void)
{
  /* Same shape as connect.c and the harness: 40 hex chars. Per-session is
   * fine; persisting it would make "this device" recognisable across
   * restarts, which matters for Connect but not for catalog reads. */
  GString *hex = g_string_new (NULL);
  GRand   *r   = g_rand_new ();
  for (int i = 0; i < 20; i++)
    g_string_append_printf (hex, "%02x", (guint8) g_rand_int_range (r, 0, 256));
  g_rand_free (r);
  return g_string_free (hex, FALSE);
}

static void flush_pending_ops (SpotifyNativeSession *self, gboolean refresh_ok);



/* This device's id, derived once and compared against the cluster's
 * active_device_id to notice a transfer. */
static gchar connect_device_id[41];

/* ── Connect: registering this client as a device ───────────────────────────
 *
 * Step three of research/connect.md. The connection id from the dealer goes in
 * the X-Spotify-Connection-Id header of an HTTPS PUT to spclient, carrying a
 * PutStateRequest. Mercury is not involved, which is what connect.c had wrong.
 *
 * Every field number below was read out of the descriptors embedded in the
 * shipped client rather than remembered -- MemberType.CONNECT_STATE_EXTENDED
 * is 5, not the 3 its position suggests, and DeviceInfo.name is 3 while brand
 * and model are 14 and 15. Guessing any of these gets an opaque HTTP 400 with
 * an empty body, the same failure the Windows client-token block had.
 */
#define CS_DEVINFO_CAN_PLAY        1
#define CS_DEVINFO_VOLUME          2
#define CS_DEVINFO_NAME            3
#define CS_DEVINFO_CAPABILITIES    4
#define CS_DEVINFO_SW_VERSION      6
#define CS_DEVINFO_DEVICE_TYPE     7
#define CS_DEVINFO_DEVICE_ID      10
#define CS_DEVINFO_CLIENT_ID      13

#define CS_CAP_CAN_BE_PLAYER      2
#define CS_CAP_VOLUME_STEPS       8
#define CS_CAP_SUPPORTED_TYPES    9
#define CS_CAP_IS_CONTROLLABLE   16
#define CS_CAP_SUPPORTS_TRANSFER 19
#define CS_CAP_SUPPORTS_COMMAND  20

#define CS_DEVICE_DEVICE_INFO     1
#define CS_DEVICE_PLAYER_STATE    2

/* PlayerState, from player.proto's descriptor at 0x87b920. Only the fields an
 * idle device needs: enough to say "here, and playing nothing". */
#define CS_PS_TIMESTAMP           1
#define CS_PS_PLAYBACK_SPEED      9
#define CS_PS_POSITION            10
#define CS_PS_IS_PLAYING          12
#define CS_PS_IS_PAUSED           13
#define CS_PS_IS_BUFFERING        14
#define CS_PS_IS_SYSTEM_INITIATED 15
#define CS_PS_CONTEXT_URI          2
#define CS_PS_TRACK                7   /* ProvidedTrack */
#define CS_PROVIDEDTRACK_URI       1

#define CS_PUT_DEVICE             2
#define CS_PUT_MEMBER_TYPE        3
#define CS_PUT_IS_ACTIVE          4
#define CS_PUT_REASON             5
#define CS_PUT_MESSAGE_ID         6
#define CS_PUT_LAST_CMD_DEVICE    7
#define CS_PUT_LAST_CMD_MSG_ID    8
#define CS_PUT_STARTED_PLAYING_AT  9
#define CS_PUT_HAS_BEEN_PLAYING_MS 11
#define CS_PUT_CLIENT_TIMESTAMP  12

#define CS_MEMBER_CONNECT_STATE   2   /* MemberType */
#define CS_REASON_NEW_DEVICE      3   /* PutStateReason */
#define CS_REASON_PLAYER_STATE_CHANGED 4
#define CS_DEVICE_TYPE_COMPUTER   1   /* devices.DeviceType */

/*
 * What this device is actually doing, as last reported by the window.
 *
 * An active device that reports an idle player state is a contradiction, and
 * the server resolves it by dropping the device -- which looks like connecting
 * successfully and then disappearing a few seconds later.
 */
static gchar   *connect_now_uri;
static gint64   connect_now_position_ms;
static gboolean connect_now_playing;

/* Set while acknowledging a transfer: the command being answered, and the
 * intention to become the active device. Cleared after one put_state. */
static gboolean  connect_claim_active;
/* When this device became the active one, in ms. Zero until it claims. */
static gint64    connect_active_since;
static gchar    *connect_ack_device;
static guint64   connect_ack_msg_id;

static GBytes *
connect_build_put_state (const gchar *device_id, const gchar *device_name,
                         const gchar *client_id)
{
  g_autoptr(GByteArray) caps = g_byte_array_new ();
  pb_write_varint_field (caps, CS_CAP_CAN_BE_PLAYER, 1);
  pb_write_varint_field (caps, CS_CAP_VOLUME_STEPS, 64);
  /* What this device will accept being handed. Audio only: no video, no
   * episodes, and nothing claimed that is not implemented. */
  pb_write_bytes_field (caps, CS_CAP_SUPPORTED_TYPES,
                        (const guint8 *) "audio/track", strlen ("audio/track"));
  pb_write_varint_field (caps, CS_CAP_IS_CONTROLLABLE, 1);
  pb_write_varint_field (caps, CS_CAP_SUPPORTS_TRANSFER, 1);
  pb_write_varint_field (caps, CS_CAP_SUPPORTS_COMMAND, 1);

  g_autoptr(GByteArray) info = g_byte_array_new ();
  pb_write_varint_field (info, CS_DEVINFO_CAN_PLAY, 1);
  pb_write_varint_field (info, CS_DEVINFO_VOLUME, 65535);
  pb_write_bytes_field (info, CS_DEVINFO_NAME,
                        (const guint8 *) device_name, strlen (device_name));
  pb_write_message_field (info, CS_DEVINFO_CAPABILITIES, caps->data, caps->len);
  pb_write_bytes_field (info, CS_DEVINFO_SW_VERSION,
                        (const guint8 *) "spotifygtk", strlen ("spotifygtk"));
  pb_write_varint_field (info, CS_DEVINFO_DEVICE_TYPE, CS_DEVICE_TYPE_COMPUTER);
  pb_write_bytes_field (info, CS_DEVINFO_DEVICE_ID,
                        (const guint8 *) device_id, strlen (device_id));
  if (client_id && *client_id)
    pb_write_bytes_field (info, CS_DEVINFO_CLIENT_ID,
                          (const guint8 *) client_id, strlen (client_id));

  /*
   * An idle player state, because a device that has never reported one may
   * simply not be listed -- device_info alone registered without appearing.
   *
   * playback_speed is a double and pb_write_varint_field cannot express it, so
   * it is left out rather than written as an integer: a wrong wire type is the
   * one thing guaranteed to be rejected, and an absent optional field is not.
   */
  g_autoptr(GByteArray) ps = g_byte_array_new ();
  pb_write_varint_field (ps, CS_PS_TIMESTAMP,
                         (guint64) (g_get_real_time () / 1000));

  /* Report the track, not silence. Claiming to be active while saying nothing
   * is playing is what made the device vanish moments after connecting. */
  if (connect_now_uri && *connect_now_uri) {
    pb_write_bytes_field (ps, CS_PS_CONTEXT_URI,
                          (const guint8 *) connect_now_uri,
                          strlen (connect_now_uri));

    g_autoptr(GByteArray) tr = g_byte_array_new ();
    pb_write_bytes_field (tr, CS_PROVIDEDTRACK_URI,
                          (const guint8 *) connect_now_uri,
                          strlen (connect_now_uri));
    pb_write_message_field (ps, CS_PS_TRACK, tr->data, tr->len);
  }

  pb_write_varint_field (ps, CS_PS_POSITION, (guint64) connect_now_position_ms);
  pb_write_varint_field (ps, CS_PS_IS_PLAYING, connect_now_playing ? 1 : 0);
  pb_write_varint_field (ps, CS_PS_IS_PAUSED, connect_now_playing ? 0 : 1);
  pb_write_varint_field (ps, CS_PS_IS_BUFFERING, 0);
  pb_write_varint_field (ps, CS_PS_IS_SYSTEM_INITIATED, 1);

  g_autoptr(GByteArray) device = g_byte_array_new ();
  pb_write_message_field (device, CS_DEVICE_DEVICE_INFO, info->data, info->len);
  pb_write_message_field (device, CS_DEVICE_PLAYER_STATE, ps->data, ps->len);

  g_autoptr(GByteArray) put = g_byte_array_new ();
  pb_write_message_field (put, CS_PUT_DEVICE, device->data, device->len);
  pb_write_varint_field (put, CS_PUT_MEMBER_TYPE, CS_MEMBER_CONNECT_STATE);
  /*
   * Announcing existence, or answering a transfer.
   *
   * A first announcement is not active: it says the device exists and can be
   * picked. Answering a transfer has to claim active and name the command it
   * is answering -- last_command_sent_by_device_id and last_command_message_id
   * are how the controller learns its command was taken, and without them the
   * phone sits on "Connecting…" waiting for an acknowledgement that never
   * comes.
   */
  pb_write_varint_field (put, CS_PUT_IS_ACTIVE, connect_claim_active ? 1 : 0);
  pb_write_varint_field (put, CS_PUT_REASON,
                         connect_claim_active ? CS_REASON_PLAYER_STATE_CHANGED
                                              : CS_REASON_NEW_DEVICE);
  /* Increments. A controller and the server both use this to tell a fresh
   * state from one they have already seen; sending 1 every time says nothing
   * has changed since the device appeared. */
  static guint64 message_id;
  pb_write_varint_field (put, CS_PUT_MESSAGE_ID, ++message_id);

  /*
   * When this device took over, and how long it has been at it.
   *
   * Both are real fields of PutStateRequest (9 and 11, read out of the
   * shipped client's descriptors) and both were missing. A device that claims
   * is_active without ever saying when it started is not promoted to
   * active_device_id: the put succeeds, the device is listed, and the
   * controller waits on "Connecting..." for an active device that never
   * arrives.
   */
  if (connect_claim_active) {
    if (connect_active_since == 0)
      connect_active_since = g_get_real_time () / 1000;
    pb_write_varint_field (put, CS_PUT_STARTED_PLAYING_AT,
                           (guint64) connect_active_since);
    pb_write_varint_field (put, CS_PUT_HAS_BEEN_PLAYING_MS,
                           (guint64) ((g_get_real_time () / 1000)
                                      - connect_active_since));
  }

  if (connect_claim_active && connect_ack_device) {
    pb_write_bytes_field (put, CS_PUT_LAST_CMD_DEVICE,
                          (const guint8 *) connect_ack_device,
                          strlen (connect_ack_device));
    pb_write_varint_field (put, CS_PUT_LAST_CMD_MSG_ID, connect_ack_msg_id);
  }
  pb_write_varint_field (put, CS_PUT_CLIENT_TIMESTAMP,
                         (guint64) (g_get_real_time () / 1000));

  return g_bytes_new (put->data, put->len);
}

#define CS_CLUSTERUPDATE_CLUSTER_FWD 1
static gchar *cluster_active_device (const guint8 *cluster, gsize len);

static void
on_put_state_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GBytes) body =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  SoupMessage *msg = user_data;
  guint status = msg ? soup_message_get_status (msg) : 0;
  const gchar *dev_id = msg ? g_object_get_data (G_OBJECT (msg), "device-id") : NULL;

  gsize len = 0;
  const gchar *d = body ? g_bytes_get_data (body, &len) : NULL;

  g_message ("[connect] put_state -> HTTP %u, %" G_GSIZE_FORMAT " byte(s) back%s",
             status, len, err ? " (transport error)" : "");
  if (err) {
    g_warning ("[connect] put_state failed: %s", err->message);
  } else if (status >= 400 && d) {
    g_warning ("[connect] body: %.*s", (int) MIN (len, 300), d);
  } else if (d) {
    /*
     * A 200 only says the request parsed, and being listed only says the
     * device exists. The claim we actually make is is_active, and the only
     * answer to it is who the returned Cluster names as the active device --
     * so say that outright rather than inferring it from presence.
     */
    /* The reply is a bare Cluster; a ClusterUpdate wrapper is accepted too,
     * because which one comes back is not worth being wrong about. */
    const guint8 *cl = NULL; gsize cllen = 0;
    if (!pb_find_bytes_field ((const guint8 *) d, len,
                              CS_CLUSTERUPDATE_CLUSTER_FWD, &cl, &cllen)) {
      cl = (const guint8 *) d;
      cllen = len;
    }
    g_autofree gchar *act = cluster_active_device (cl, cllen);
    if (!act || !*act) {
      g_autofree gchar *bare = cluster_active_device ((const guint8 *) d, len);
      if (bare && *bare) { g_free (act); act = g_steal_pointer (&bare); }
    }
    g_message ("[connect] server says active=%s (%s)",
               act && *act ? act : "(none)",
               g_strcmp0 (act, dev_id) == 0 ? "US" : "not us");

    /*
     * A 200 only says the request parsed. The answer is a Cluster, so the
     * question worth asking is whether we are in it -- the device name is
     * plain in the protobuf, so a substring search settles it without
     * decoding the whole thing.
     */
    /*
     * memmem, not g_strstr_len.
     *
     * The body is protobuf: 35 KB of binary with NUL bytes throughout, and the
     * device entry is near the end of it. g_strstr_len is for strings and does
     * not find a needle past the first NUL, so it answered "absent" for five
     * rounds of debugging while the device was registering perfectly. The
     * cluster was right; the check was not.
     */
    gboolean by_name = memmem (d, len, "SpotifyGTK", strlen ("SpotifyGTK")) != NULL;
    gboolean by_id   = dev_id && memmem (d, len, dev_id, strlen (dev_id)) != NULL;
    g_message ("[connect] our device in the returned cluster: by name %s, by id %s",
               by_name ? "yes" : "no", by_id ? "yes" : "no");

    if (g_getenv ("SPOTIFY_CLUSTER_DUMP"))
      g_file_set_contents (g_getenv ("SPOTIFY_CLUSTER_DUMP"), d, (gssize) len, NULL);

    if (g_getenv ("SPOTIFY_PROBE_CLUSTER")) {
      GString *run = g_string_new (NULL);
      guint shown = 0;
      for (gsize i = 0; i < len && shown < 30; i++) {
        if (g_ascii_isprint (d[i])) {
          g_string_append_c (run, d[i]);
        } else {
          if (run->len >= 6) { g_message ("[cluster] %s", run->str); shown++; }
          g_string_truncate (run, 0);
        }
      }
      g_string_free (run, TRUE);
    }
  }

  g_clear_object (&msg);
}

static void
connect_put_state (const gchar *bearer, const gchar *connection_id)
{
  /* 40 hex characters, which is the shape every real device id has -- the
   * first attempt used a readable string of the right length and the server
   * took the request but did not put the device in the cluster. */
  /*
   * sizeof the array, not the pointer.
   *
   * Hoisting this to file scope turned `static gchar device_id[41]` into a
   * `gchar *`, and `sizeof device_id` quietly became 8 -- so g_strlcpy wrote
   * seven hex characters and a NUL, and the client registered itself as
   * "86dc1e0". The server accepted it and echoed it back, so the check that
   * looks for our id in the returned cluster still said yes.
   */
  gchar *device_id = connect_device_id;
  if (!device_id[0]) {
    g_autoptr(GChecksum) sum = g_checksum_new (G_CHECKSUM_SHA1);
    g_checksum_update (sum, (const guchar *) bearer, 32);
    g_strlcpy (device_id, g_checksum_get_string (sum), sizeof connect_device_id);
  }
  g_autofree gchar *url =
    g_strdup_printf ("https://gae2-spclient.spotify.com/connect-state/v1/devices/%s",
                     device_id);

  /*
   * Spotify's own client id, the one sign-in already uses -- not a
   * developer-portal registration. A portal id is scoped to the Web API, which
   * is a different surface from spclient, so it would likely be refused here;
   * and requiring one would mean everybody registering an app before their
   * speakers showed up, which is not a thing this client should ask for.
   */
  g_autoptr(GBytes) body =
    connect_build_put_state (device_id, "SpotifyGTK", NATIVE_AUTH_CLIENT_ID);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_PUT, url);
  SoupMessageHeaders *h = soup_message_get_request_headers (msg);
  g_autofree gchar *auth = g_strdup_printf ("Bearer %s", bearer);
  soup_message_headers_replace (h, "Authorization", auth);
  soup_message_headers_replace (h, "X-Spotify-Connection-Id", connection_id);
  soup_message_headers_replace (h, "Content-Type", "application/protobuf");
  soup_message_set_request_body_from_bytes (msg, "application/protobuf", body);

  gsize blen = 0;
  g_bytes_get_data (body, &blen);
  g_message ("[connect] PUT %s (%" G_GSIZE_FORMAT " byte PutStateRequest)",
             url, blen);

  SoupSession *s = soup_session_new ();
  g_object_set_data_full (G_OBJECT (msg), "device-id", g_strdup (device_id), g_free);
  soup_session_send_and_read_async (s, msg, G_PRIORITY_DEFAULT, NULL,
                                    on_put_state_done, msg);
}

/* ── Probe: can a third-party client hold a dealer connection? ──────────────
 *
 * The gate for Spotify Connect. Registration is an HTTPS PUT carrying an
 * X-Spotify-Connection-Id header, and that id only exists if the dealer will
 * talk to us -- so whether any of the rest is worth writing is settled here
 * and nowhere else. See research/connect.md.
 *
 * Read-only: it opens a socket, reads what arrives and says so. Nothing is
 * registered, nothing is written, no device is announced.
 */
static SoupWebsocketConnection *dealer_ws;   /* probe only; see connect.md */
/* The session the dealer speaks for, so an incoming command can be turned
 * into a signal. Weak: it is a borrowed pointer, and a command arriving after
 * the session is gone must find NULL rather than free'd memory. */
static SpotifyNativeSession    *dealer_session;
static void dealer_connect (const gchar *bearer);
static gchar                   *dealer_redial_bearer;
static guint                    dealer_redial_delay = 1;
static gchar *dealer_conn_id;
static gchar *dealer_bearer;

/*
 * The dealer's heartbeat is JSON, {"type":"ping"} and {"type":"pong"}, sitting
 * beside the string "heartbeat" in the shipped client. Nothing ever pinged us
 * across fifty seconds, so the client is the one expected to start it -- and a
 * connection the server treats as dead is a plausible reason for a device to
 * be accepted and then never listed.
 */
static gboolean
dealer_ping (gpointer user_data)
{
  (void) user_data;
  if (!dealer_ws ||
      soup_websocket_connection_get_state (dealer_ws) != SOUP_WEBSOCKET_STATE_OPEN)
    return G_SOURCE_REMOVE;

  soup_websocket_connection_send_text (dealer_ws, "{\"type\":\"ping\"}");
  g_message ("[dealer] ping sent");
  return G_SOURCE_CONTINUE;
}




/* ── TransferState, from transfer_state.proto (0x8a4700) and friends ────────
 *
 *   TransferState  playback=2  current_session=3
 *   Playback       timestamp=1  position_as_of_timestamp=2  is_paused=4
 *                  current_track=5 (ContextTrack)
 *   Session        context=2 (Context)
 *   Context        uri=1
 *   ContextTrack   uri=1
 *
 * Enough to answer the only three questions playback needs: which track, from
 * where, and playing or paused.
 */
#define TS_PLAYBACK            2
#define TS_SESSION             3
#define PB_TIMESTAMP           1
#define PB_POSITION            2
#define PB_IS_PAUSED           4
#define PB_CURRENT_TRACK       5
#define SESSION_CONTEXT        2
#define CTX_URI                1
#define CTXTRACK_URI           1
#define CTXTRACK_GID           3

static void
dealer_handle_transfer_state (SpotifyNativeSession *self,
                              const guint8 *data, gsize len)
{
  const guint8 *v = NULL; gsize vlen = 0;
  g_autofree gchar *track_uri = NULL;
  g_autofree gchar *context_uri = NULL;
  gint64 position_ms = 0;
  gboolean paused = FALSE;

  if (pb_find_bytes_field (data, len, TS_PLAYBACK, &v, &vlen)) {
    const guint8 *t = NULL; gsize tlen = 0;
    if (pb_find_bytes_field (v, vlen, PB_CURRENT_TRACK, &t, &tlen)) {
      const guint8 *u = NULL; gsize ulen = 0;

      /*
       * A transfer names the track by gid, not by uri.
       *
       * ContextTrack has a uri field and it is empty here: a real transfer
       * carried uid and gid only. Reading field 1 alone finds nothing and
       * reports there is nothing to play, having just claimed the device --
       * the worst of both. The gid is the same 16 bytes every other id in
       * this codebase base62-encodes.
       */
      if (pb_find_bytes_field (t, tlen, CTXTRACK_URI, &u, &ulen) && ulen > 0) {
        track_uri = g_strndup ((const gchar *) u, ulen);
      } else if (pb_find_bytes_field (t, tlen, CTXTRACK_GID, &u, &ulen)) {
        g_autofree gchar *b62 = spotifygtk_gid_to_base62 (u, ulen);
        if (b62)
          track_uri = g_strdup_printf ("spotify:track:%s", b62);
      }
    }

    gsize pos = 0; guint32 fn; PbWireType wt;
    const guint8 *fd; gsize fl; guint64 fv;
    while (pb_read_field (v, vlen, &pos, &fn, &wt, &fd, &fl, &fv)) {
      if (fn == PB_POSITION && wt == PB_WIRE_VARINT)
        position_ms = (gint64) fv;
      else if (fn == PB_IS_PAUSED && wt == PB_WIRE_VARINT)
        paused = (fv != 0);
    }
  }

  if (pb_find_bytes_field (data, len, TS_SESSION, &v, &vlen)) {
    const guint8 *ctx = NULL; gsize clen = 0;
    if (pb_find_bytes_field (v, vlen, SESSION_CONTEXT, &ctx, &clen)) {
      const guint8 *u = NULL; gsize ulen = 0;
      if (pb_find_bytes_field (ctx, clen, CTX_URI, &u, &ulen))
        context_uri = g_strndup ((const gchar *) u, ulen);
    }
  }

  g_message ("[transfer] track=%s position=%" G_GINT64_FORMAT "ms paused=%s "
             "context=%s",
             track_uri ? track_uri : "(none)", position_ms,
             paused ? "yes" : "no", context_uri ? context_uri : "(none)");

  /*
   * An empty transfer is normal, not a failure.
   *
   * A phone with nothing playing hands over nothing: 83 bytes, a timestamp, a
   * position of zero and no current_track at all. The device should become
   * active and wait, exactly as a speaker does when selected while idle. What
   * it must not do is treat this as an error and sit silent, because the
   * controller then has an active device that never reports anything.
   */
  if (!track_uri) {
    g_message ("[transfer] nothing was playing on the other device; taking the "
               "device idle and waiting for a play command");
    return;
  }

  if (!self) {
    g_warning ("[transfer] no session to hand the track to");
    return;
  }

  emit_remote (self, FALSE, track_uri, position_ms, paused);
}

/*
 * A command from a controller.
 *
 * Distinct from a cluster update, and shaped differently: "payload" singular,
 * an object rather than an array, under a message_ident of
 * hm://connect-state/v1/player/command. Tapping this device in another
 * client's picker sends endpoint "transfer".
 *
 * The reply is a put_state that claims active and names the command being
 * answered. Until that arrives the controller shows "Connecting…" and
 * eventually gives up, which is exactly what it did.
 */
static void
dealer_handle_command (JsonObject *root)
{
  JsonObject *payload = json_object_get_object_member (root, "payload");
  if (!payload)
    return;

  JsonObject *cmd = json_object_has_member (payload, "command")
                      ? json_object_get_object_member (payload, "command") : NULL;
  const gchar *endpoint = cmd && json_object_has_member (cmd, "endpoint")
                            ? json_object_get_string_member (cmd, "endpoint") : NULL;

  const gchar *from = json_object_has_member (payload, "sent_by_device_id")
                        ? json_object_get_string_member (payload, "sent_by_device_id") : NULL;
  guint64 msg_id = json_object_has_member (payload, "message_id")
                     ? (guint64) json_object_get_int_member (payload, "message_id") : 0;

  g_message ("[command] %s from %s (message_id %" G_GUINT64_FORMAT ")",
             endpoint ? endpoint : "(none)", from ? from : "(unknown)", msg_id);

  if (!dealer_bearer || !dealer_conn_id) {
    g_warning ("[command] arrived with no connection id to answer on");
    return;
  }

  /*
   * Every command is acknowledged, whether or not it is understood.
   *
   * A controller that gets no answer treats the device as unresponsive and
   * takes playback back, which looks from here like connecting and then being
   * dropped. Answering an endpoint we do nothing with is still better than
   * silence -- it keeps the device alive while the rest is built.
   */
  g_free (connect_ack_device);
  connect_ack_device   = g_strdup (from);
  connect_ack_msg_id   = msg_id;
  connect_claim_active = TRUE;

  if (g_strcmp0 (endpoint, "transfer") != 0) {
    gint64 seek_to = 0;
    if (g_strcmp0 (endpoint, "seek_to") == 0 && cmd &&
        json_object_has_member (cmd, "value"))
      seek_to = json_object_get_int_member (cmd, "value");

    g_message ("[command] acknowledging %s", endpoint ? endpoint : "(none)");
    connect_put_state (dealer_bearer, dealer_conn_id);
    emit_remote (dealer_session, TRUE, endpoint, seek_to, FALSE);
    return;
  }

  g_message ("[command] answering the transfer: put_state with is_active=1");
  connect_put_state (dealer_bearer, dealer_conn_id);

  /* The command carries what to play, base64 in "data". Acknowledging without
   * this claims the device and then sits silent, which is worse than not
   * claiming it. */
  const gchar *b64 = cmd && json_object_has_member (cmd, "data")
                       ? json_object_get_string_member (cmd, "data") : NULL;
  if (!b64) {
    g_warning ("[command] transfer carried no data; claimed the device but "
               "have nothing to play");
    return;
  }

  gsize raw_len = 0;
  g_autofree guchar *raw = g_base64_decode (b64, &raw_len);
  if (raw && raw_len)
    dealer_handle_transfer_state (dealer_session, raw, raw_len);
}

/* ── Reading what the dealer pushes ─────────────────────────────────────────
 *
 * Cluster updates arrive unasked on the socket already held open: a JSON
 * envelope whose "payloads" array holds base64, which decodes to a
 * ClusterUpdate. Field numbers from research/connect.md, read out of the
 * shipped client's descriptors.
 *
 * The whole message is parsed, not the truncated copy used for the log line --
 * these run to 38 KB and the interesting part is nowhere near the front.
 */
#define CS_CLUSTERUPDATE_CLUSTER   1
#define CS_CLUSTER_ACTIVE_DEVICE   2
#define CS_CLUSTER_PLAYER_STATE    3
#define CS_PLAYERSTATE_CONTEXT_URI 2
#define CS_PLAYERSTATE_IS_PLAYING  12

/* Who the server says is active, read straight out of a Cluster. */
static gchar *
cluster_active_device (const guint8 *cluster, gsize len)
{
  const guint8 *v = NULL; gsize vlen = 0;
  if (pb_find_bytes_field (cluster, len, CS_CLUSTER_ACTIVE_DEVICE, &v, &vlen))
    return g_strndup ((const gchar *) v, vlen);
  return NULL;
}

static void
dealer_handle_cluster (const guint8 *cluster, gsize len)
{
  g_autofree gchar *active = NULL;
  const guint8 *v = NULL; gsize vlen = 0;

  if (pb_find_bytes_field (cluster, len, CS_CLUSTER_ACTIVE_DEVICE, &v, &vlen))
    active = g_strndup ((const gchar *) v, vlen);

  g_autofree gchar *context = NULL;
  gboolean playing = FALSE;
  if (pb_find_bytes_field (cluster, len, CS_CLUSTER_PLAYER_STATE, &v, &vlen)) {
    const guint8 *c2 = NULL; gsize c2len = 0;
    if (pb_find_bytes_field (v, vlen, CS_PLAYERSTATE_CONTEXT_URI, &c2, &c2len))
      context = g_strndup ((const gchar *) c2, c2len);

    gsize pos = 0; guint32 fn; PbWireType wt;
    const guint8 *fd; gsize fl; guint64 fv;
    while (pb_read_field (v, vlen, &pos, &fn, &wt, &fd, &fl, &fv))
      if (fn == CS_PLAYERSTATE_IS_PLAYING && wt == PB_WIRE_VARINT)
        playing = (fv != 0);
  }

  gboolean ours = active && connect_device_id[0] &&
                  g_strcmp0 (active, connect_device_id) == 0;

  g_message ("[cluster] active=%s%s playing=%s context=%s",
             active ? active : "(none)", ours ? "  <-- THIS DEVICE" : "",
             playing ? "yes" : "no", context ? context : "(none)");

  if (ours)
    g_message ("[cluster] TRANSFER: another client has handed playback here. "
               "Taking it needs a put_state with is_active=1 and the context "
               "given to the player -- not built yet.");
}

static void
dealer_handle_payloads (const gchar *body, gsize len)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body, (gssize) len, NULL))
    return;

  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    return;

  JsonObject *obj = json_node_get_object (root);

  /* Commands come as "payload" (an object); cluster updates as "payloads"
   * (an array of base64). Different shapes, same socket. */
  const gchar *ident = json_object_has_member (obj, "message_ident")
                         ? json_object_get_string_member (obj, "message_ident") : NULL;
  if (ident && strstr (ident, "player/command")) {
    dealer_handle_command (obj);
    return;
  }

  if (!json_object_has_member (obj, "payloads"))
    return;

  JsonNode *pn = json_object_get_member (obj, "payloads");
  if (!JSON_NODE_HOLDS_ARRAY (pn))
    return;

  JsonArray *arr = json_node_get_array (pn);
  for (guint i = 0; i < json_array_get_length (arr); i++) {
    /* social-connect sends objects in this same array; asking those for a
     * string is a Json-CRITICAL and, before this check, a real one in the log. */
    JsonNode *el = json_array_get_element (arr, i);
    if (!el || !JSON_NODE_HOLDS_VALUE (el))
      continue;
    const gchar *b64 = json_node_get_string (el);
    if (!b64)
      continue;

    gsize raw_len = 0;
    g_autofree guchar *raw = g_base64_decode (b64, &raw_len);
    if (!raw || raw_len == 0)
      continue;

    /* ClusterUpdate.cluster, or nothing we understand yet. */
    const guint8 *cluster = NULL; gsize clen = 0;
    if (pb_find_bytes_field (raw, raw_len, CS_CLUSTERUPDATE_CLUSTER,
                             &cluster, &clen))
      dealer_handle_cluster (cluster, clen);
    else
      g_message ("[cluster] payload %u (%" G_GSIZE_FORMAT " bytes) carried no "
                 "cluster; not a ClusterUpdate", i, raw_len);
  }
}


/*
 * Self-test for the cluster parse.
 *
 * Waiting for a real update means waiting for someone to press play on another
 * device, which is a poor way to find out whether the field numbers are right.
 * This builds a ClusterUpdate with known contents, wraps it exactly as the
 * dealer does -- base64 inside a JSON payloads array -- and pushes it through
 * the same entry point a real message takes.
 */
static void
dealer_cluster_selftest (void)
{
  g_autoptr(GByteArray) ps = g_byte_array_new ();
  pb_write_bytes_field (ps, CS_PLAYERSTATE_CONTEXT_URI,
                        (const guint8 *) "spotify:playlist:selftest",
                        strlen ("spotify:playlist:selftest"));
  pb_write_varint_field (ps, CS_PLAYERSTATE_IS_PLAYING, 1);

  g_autoptr(GByteArray) cl = g_byte_array_new ();
  pb_write_varint_field (cl, 1, 1786904386515ULL);          /* changed_timestamp_ms */
  pb_write_bytes_field (cl, CS_CLUSTER_ACTIVE_DEVICE,
                        (const guint8 *) connect_device_id,
                        strlen (connect_device_id));
  pb_write_message_field (cl, CS_CLUSTER_PLAYER_STATE, ps->data, ps->len);

  g_autoptr(GByteArray) cu = g_byte_array_new ();
  pb_write_message_field (cu, CS_CLUSTERUPDATE_CLUSTER, cl->data, cl->len);

  g_autofree gchar *b64 = g_base64_encode (cu->data, cu->len);
  g_autofree gchar *json =
    g_strdup_printf ("{\"payloads\":[\"%s\"],\"type\":\"message\"}", b64);

  g_message ("[cluster-selftest] feeding a %u-byte ClusterUpdate naming this "
             "device as active", cu->len);
  dealer_handle_payloads (json, strlen (json));
}

/*
 * Keep saying we are here.
 *
 * This began as a one-shot, to find out whether a second announcement over a
 * live socket behaved differently from the first. It has to be periodic: a
 * device that registers and then stops reporting state gets dropped, and the
 * symptom is precisely a device that appears in a phone's picker, vanishes,
 * comes back once -- when the one-shot fired -- and then stays gone.
 *
 * Thirty seconds, matching the ping. A real client also puts state on every
 * change; that comes with actually having state to report.
 */
static gboolean
dealer_reput (gpointer user_data)
{
  static guint n;
  (void) user_data;

  if (!dealer_bearer || !dealer_conn_id)
    return G_SOURCE_REMOVE;

  g_message ("[connect] keepalive put_state (%u)", ++n);
  connect_put_state (dealer_bearer, dealer_conn_id);
  return G_SOURCE_CONTINUE;
}

/* Reply to a dealer request, if that is what this frame is. */
static void
dealer_reply_if_request (SoupWebsocketConnection *ws, const gchar *body,
                         gsize len)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body, (gssize) len, NULL))
    return;

  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    return;

  JsonObject *obj = json_node_get_object (root);
  if (!json_object_has_member (obj, "key")) {
    if (memmem (body, len, "player/command", strlen ("player/command")))
      g_message ("[dealer] command frame carried no key; nothing to reply to");
    return;
  }

  const gchar *type = json_object_has_member (obj, "type")
                        ? json_object_get_string_member (obj, "type") : NULL;
  if (g_strcmp0 (type, "request") != 0) {
    g_message ("[dealer] frame has a key but type is %s; not replying",
               type ? type : "(absent)");
    return;
  }

  const gchar *key = json_object_get_string_member (obj, "key");
  if (!key)
    return;

  g_autofree gchar *reply =
    g_strdup_printf ("{\"type\":\"reply\",\"key\":\"%s\","
                     "\"payload\":{\"success\":true}}", key);
  soup_websocket_connection_send_text (ws, reply);
  g_message ("[dealer] replied success to request key %s", key);
}

static void
on_dealer_message (SoupWebsocketConnection *ws, gint type, GBytes *message,
                   gpointer user_data)
{
  gsize len = 0;
  const gchar *d = g_bytes_get_data (message, &len);
  g_autofree gchar *text = g_strndup (d, MIN (len, 700));
  (void) ws; (void) type; (void) user_data;

  g_message ("[dealer] message (%" G_GSIZE_FORMAT " bytes): %s", len, text);

  /* Parsed from the full body; `text` above is only ever a log line. */
  if (memmem (d, len, "\"payload", strlen ("\"payload")))
    dealer_handle_payloads (d, len);

  /*
   * Answer the request on the socket it came in on.
   *
   * A command is not a notification: the dealer sends it as a request with a
   * key and waits for a reply frame naming that key. Answering only with a
   * put_state over HTTPS -- which is a different connection, and which the
   * server has no way to match to this request -- leaves the request
   * outstanding, and the dealer drops the connection about three seconds
   * later. Every observed disconnect was 3.1-3.2s after a transfer, and runs
   * that received no command stayed up indefinitely.
   */
  dealer_reply_if_request (ws, d, len);

  /* Answer a ping if one ever arrives, whichever way round it turns out to be. */
  if (g_strstr_len (text, -1, "\"ping\"")) {
    soup_websocket_connection_send_text (ws, "{\"type\":\"pong\"}");
    g_message ("[dealer] pong sent");
  }

  /*
   * From the header, not the uri.
   *
   * Both carry it, but the uri's copy is URL-encoded -- the trailing "==" of
   * the base64 arrives as %3D%3D -- so taking it from there means decoding it
   * back before it can go in a header. The headers copy is already the value
   * the X-Spotify-Connection-Id request header wants.
   */
  const gchar *key = "\"Spotify-Connection-Id\":\"";
  const gchar *p = g_strstr_len (text, -1, key);
  if (p) {
    p += strlen (key);
    const gchar *end = strchr (p, '"');
    if (end) {
      g_autofree gchar *id = g_strndup (p, (gsize) (end - p));
      g_message ("[dealer] CONNECTION ID (%" G_GSIZE_FORMAT " chars): %s",
                 strlen (id), id);
      if (g_getenv ("SPOTIFY_PROBE_PUTSTATE")) {
        g_free (dealer_conn_id);
        dealer_conn_id = g_strdup (id);
        g_free (dealer_bearer);
        dealer_bearer = g_strdup ((const gchar *) user_data);
        connect_put_state (dealer_bearer, dealer_conn_id);
        if (g_getenv ("SPOTIFY_CLUSTER_SELFTEST"))
          dealer_cluster_selftest ();
        g_timeout_add_seconds (30, dealer_ping, NULL);
        g_timeout_add_seconds (30, dealer_reput, NULL);
      }
    }
  }
}

static gboolean
dealer_redial (gpointer user_data)
{
  (void) user_data;
  if (dealer_redial_bearer) {
    g_message ("[dealer] redialling");
    dealer_connect (dealer_redial_bearer);
  }
  return G_SOURCE_REMOVE;
}

/*
 * A closed socket is not the end of the device.
 *
 * This used to log and stop, so the first disconnect -- for any reason --
 * removed the device from every picker until the app was restarted. The
 * connection id dies with the socket, so a redial has to re-register from
 * scratch, which on_dealer_connected already does.
 */
static void
on_dealer_closed (SoupWebsocketConnection *ws, gpointer user_data)
{
  (void) user_data;
  g_message ("[dealer] closed: %d %s",
             soup_websocket_connection_get_close_code (ws),
             soup_websocket_connection_get_close_data (ws));

  g_clear_object (&dealer_ws);

  g_message ("[dealer] reconnecting in %us", dealer_redial_delay);
  g_timeout_add_seconds (dealer_redial_delay, dealer_redial, NULL);
  dealer_redial_delay = MIN (dealer_redial_delay * 2, 60);
}

static void
on_dealer_connected (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GError) err = NULL;
  SoupWebsocketConnection *ws =
    soup_session_websocket_connect_finish (SOUP_SESSION (source), result, &err);
  (void) user_data;

  if (!ws) {
    g_warning ("[dealer] connect failed: %s", err ? err->message : "unknown");
    return;
  }

  g_message ("[dealer] connected -- the dealer accepted this client");
  dealer_redial_delay = 1;
  g_clear_object (&dealer_ws);
  dealer_ws = g_object_ref (ws);
  g_signal_connect (ws, "message", G_CALLBACK (on_dealer_message), user_data);
  g_signal_connect (ws, "closed",  G_CALLBACK (on_dealer_closed), NULL);
  /* Deliberately leaked for the length of the probe: dropping the last
   * reference closes the socket, and the point is to see what it sends. */
}

/* Dial the dealer. Separate from dealer_probe so a dropped socket can be
 * redialled without going through the one-shot guard again. */
static void
dealer_connect (const gchar *bearer)
{
  if (!bearer)
    return;

  /* One host is enough to answer the question; apresolve returns four and
   * they are equivalent. */
  g_autofree gchar *url =
    g_strdup_printf ("wss://gae2-dealer.spotify.com/?access_token=%s", bearer);

  SoupSession *ws_session = soup_session_new ();
  g_autoptr(SoupMessage) msg = soup_message_new (SOUP_METHOD_GET, url);
  if (!msg) {
    g_warning ("[dealer] could not build the request");
    return;
  }


  g_message ("[dealer] connecting to gae2-dealer.spotify.com");
  soup_session_websocket_connect_async (ws_session, msg, NULL, NULL,
                                        G_PRIORITY_DEFAULT, NULL,
                                        on_dealer_connected, g_strdup (bearer));
}

static void
dealer_probe (SpotifyNativeSession *self, const gchar *bearer)
{
  static gboolean done;
  if (done || !bearer)
    return;
  done = TRUE;

  /*
   * Remember who to emit on.
   *
   * Without this the pointer stayed NULL and every transfer was decoded
   * correctly and then emitted into nothing -- g_signal_emit on NULL warns and
   * returns, so the track, position and pause state were all read off the wire
   * and dropped on the floor. The device claimed playback and never played.
   */
  dealer_session = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &dealer_session);

  g_free (dealer_redial_bearer);
  dealer_redial_bearer = g_strdup (bearer);

  dealer_connect (bearer);
}

static void
on_login5_result (const gchar *access_token, gint32 expires_in_seconds,
                  GError *error, gpointer user_data)
{
  SpotifyNativeSession *self = user_data;
  gboolean was_refresh = self->refreshing;

  self->refreshing = FALSE;

  if (!access_token) {
    g_autofree gchar *msg = g_strdup_printf ("login5 failed: %s",
      error ? error->message : "unknown error");
    if (was_refresh) {
      /* A failed *refresh* is not the same as a failed sign-in: the AP
       * session is still up, so report it and let the next request retry
       * rather than tearing the whole session down. */
      g_warning ("session: bearer refresh failed: %s", msg);
      flush_pending_ops (self, FALSE);
      return;
    }
    fail_session (self, msg);
    return;
  }

  g_mutex_lock (&self->lock);
  g_free (self->bearer_token);
  self->bearer_token = g_strdup (access_token);
  /* Track expiry so requests can re-mint before the server starts
   * rejecting them. Without this the session keeps reporting READY while
   * every call 401s -- a silent death about an hour in. */
  self->bearer_expires_at = g_get_monotonic_time ()
                          + (gint64) expires_in_seconds * G_USEC_PER_SEC;
  g_mutex_unlock (&self->lock);

  if (g_getenv ("SPOTIFY_PROBE_DEALER"))
    dealer_probe (self, access_token);

  if (was_refresh) {
    g_message ("session: bearer refreshed (expires in %ds)", expires_in_seconds);
    flush_pending_ops (self, TRUE);
    return;
  }

  /* Created on the worker thread so its SoupSession binds to the worker's
   * context, not GTK's. */
  self->spclient = spotifygtk_spclient_new ();
  spotifygtk_spclient_set_cancellable (self->spclient, self->cancellable);

  g_message ("session: ready (bearer expires in %ds)", expires_in_seconds);
  set_state (self, SPOTIFYGTK_SESSION_READY, "Signed in.");
  if (g_getenv ("SPOTIFY_DUMP_TOKENS")) {
    g_autofree gchar *t = g_strdup_printf ("%s\n%s\n",
      self->bearer_token ? self->bearer_token : "", 
      self->client_token ? self->client_token : "");
    g_file_set_contents ("/tmp/claude-1000/-home-wrldmk2--SpotifyGTK/cc964017-47a6-4c10-b489-3031a013748f/scratchpad/tokens.txt", t, -1, NULL);
  }
}

/* Re-mint the bearer using the AP session's reusable credentials, which are
 * still held -- this is the same login5 call the initial sign-in makes, not
 * a fresh AP handshake. */
static void
start_bearer_refresh (SpotifyNativeSession *self)
{
  if (self->refreshing)
    return;

  gsize         creds_len  = 0;
  const guint8 *creds      = spotifygtk_ap_session_get_reusable_creds (self->ap, &creds_len);
  guint64       creds_type = spotifygtk_ap_session_get_reusable_creds_type (self->ap);
  const gchar  *username   = spotifygtk_ap_session_get_username (self->ap);

  if (!creds) {
    g_warning ("session: cannot refresh bearer -- no reusable credentials");
    flush_pending_ops (self, FALSE);
    return;
  }

  g_mutex_lock (&self->lock);
  g_autofree gchar *ctoken = g_strdup (self->client_token);
  g_mutex_unlock (&self->lock);

  self->refreshing = TRUE;
  g_message ("session: bearer expired or expiring, re-minting via login5");

  g_clear_object (&self->login5_client);
  self->login5_client = spotifygtk_login5_new ();
  spotifygtk_login5_set_cancellable (self->login5_client, self->cancellable);
  spotifygtk_login5_auth_token (self->login5_client,
                                NATIVE_AUTH_CLIENT_ID, self->device_id,
                                username, creds, creds_len, creds_type,
                                ctoken, on_login5_result, self);
}

static void
on_client_token_result (const gchar *token, gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  if (token) {
    g_mutex_lock (&self->lock);
    g_free (self->client_token);
    self->client_token = g_strdup (token);
    g_mutex_unlock (&self->lock);
  } else {
    /* login5 requires one per spclient.rs; continue so the failure surfaces
     * as a login5 error rather than being masked here. */
    g_warning ("session: no client-token obtained; continuing to login5 anyway");
  }

  const gchar  *username   = spotifygtk_ap_session_get_username (self->ap);
  gsize         creds_len  = 0;
  const guint8 *creds      = spotifygtk_ap_session_get_reusable_creds (self->ap, &creds_len);
  guint64       creds_type = spotifygtk_ap_session_get_reusable_creds_type (self->ap);

  if (!creds) {
    fail_session (self, "no reusable credentials in APWelcome; cannot reach login5");
    return;
  }

  g_mutex_lock (&self->lock);
  g_free (self->username);
  self->username = g_strdup (username);
  g_mutex_unlock (&self->lock);

  self->login5_client = spotifygtk_login5_new ();
  spotifygtk_login5_set_cancellable (self->login5_client, self->cancellable);
  spotifygtk_login5_auth_token (self->login5_client,
                                NATIVE_AUTH_CLIENT_ID, self->device_id,
                                username, creds, creds_len, creds_type,
                                token, on_login5_result, self);
}

static void
on_ap_login_result (gboolean success, const gchar *username,
                    GError *error, gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  if (!success) {
    g_autofree gchar *msg = g_strdup_printf ("AP login failed: %s",
      error ? error->message : "unknown error");
    fail_session (self, msg);
    return;
  }

  g_message ("session: AP login succeeded%s", username ? "" : " (no username returned)");
  set_state (self, SPOTIFYGTK_SESSION_CONNECTING, "Signed in; authorizing streaming session.");

  self->client_token_client = spotifygtk_client_token_new ();
  spotifygtk_client_token_set_cancellable (self->client_token_client, self->cancellable);
  spotifygtk_client_token_request (self->client_token_client,
                                   NATIVE_AUTH_CLIENT_ID, self->device_id,
                                   on_client_token_result, self);
}

/* Spotify refuses new AP connections when a client opens them too quickly.
 * Because the playback engine still logs in per track, a listening session
 * can burn through its allowance and leave even this sign-in refused, so a
 * single "Connection refused" is not a reason to give up. */
#define AP_CONNECT_MAX_ATTEMPTS 4

static void on_ap_connected (GObject *source, GAsyncResult *result, gpointer user_data);
static gboolean begin_signin (gpointer user_data);
static void collection_drain_waiters (SpotifyNativeSession *self,
                                      GPtrArray *tracks, const gchar *message);

static gboolean
retry_ap_connect (gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  if (g_cancellable_is_cancelled (self->cancellable))
    return G_SOURCE_REMOVE;

  spotifygtk_ap_session_connect (self->ap, NULL, on_ap_connected, self);
  return G_SOURCE_REMOVE;
}

static void
on_ap_connected (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyApSession     *ap   = SPOTIFYGTK_AP_SESSION (source);
  SpotifyNativeSession *self = user_data;
  g_autoptr(GError)     err  = NULL;

  if (!spotifygtk_ap_session_connect_finish (ap, result, &err)) {
    if (self->connect_attempts < AP_CONNECT_MAX_ATTEMPTS &&
        !g_cancellable_is_cancelled (self->cancellable)) {
      self->connect_attempts++;
      guint delay_ms = 500u * self->connect_attempts;
      g_warning ("session: AP connect failed (%s); retry %u/%u in %ums",
                 err ? err->message : "unknown error",
                 self->connect_attempts, AP_CONNECT_MAX_ATTEMPTS, delay_ms);

      /* Attach to the session's own context, not the global default one that
       * g_timeout_add() uses — otherwise the retry runs on the GTK main thread
       * and every request chained off it (client-token, login5, spclient) binds
       * to the main context, which is exactly the isolation this class exists
       * to guarantee. See the matching note in main.c. */
      GSource *retry = g_timeout_source_new (delay_ms);
      g_source_set_callback (retry, retry_ap_connect, self, NULL);
      g_source_attach (retry, self->context);
      g_source_unref (retry);
      return;
    }

    g_autofree gchar *msg = g_strdup_printf ("AP handshake failed: %s",
      err ? err->message : "unknown error");
    fail_session (self, msg);
    return;
  }

  spotifygtk_ap_session_start_receiving (ap);
  set_state (self, SPOTIFYGTK_SESSION_CONNECTING, "Secure channel established; signing in.");

  spotifygtk_ap_session_login (ap, g_getenv ("SPOTIFY_USERNAME"),
                               self->login_token, on_ap_login_result, self);
}

/* ── Worker thread ───────────────────────────────────────────────────────── */

static gboolean
begin_signin (gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  self->ap = spotifygtk_ap_session_new ();
  spotifygtk_ap_session_set_cancellable (self->ap, self->cancellable);
  spotifygtk_ap_session_connect (self->ap, NULL, on_ap_connected, self);

  return G_SOURCE_REMOVE;
}

/*
 * Obtain a fresh access token for AP login. Must run on the session context:
 * it can block, and it drives an async refresh through a nested loop.
 *
 * Always re-reads from NativeAuth rather than trusting what is already in
 * login_token. The AP rejects an expired token with BadCredentials, and the
 * token lives an hour while a session lives as long as the window does -- so
 * on any sign-in after the first, the cached one is more likely stale than
 * good.
 */
static gboolean
acquire_login_token (SpotifyNativeSession *self)
{
  /* Token acquisition can block (it may run a browser flow), so it happens on
   * the session context rather than the caller's thread. */
  NativeAuth *auth = native_auth_new ();
  if (!native_auth_has_valid_token (auth)) {
    /* native_auth_refresh() handles both cases: refresh a stored-but-expired
     * token, or fall through to the browser flow when there is none. It is
     * async, so drive it on this thread's context via its own nested loop. */
    GMainLoop *token_loop = g_main_loop_new (self->context, FALSE);
    gulong handler = g_signal_connect_swapped (auth, "completed",
                                               G_CALLBACK (g_main_loop_quit), token_loop);
    native_auth_refresh (auth);
    g_main_loop_run (token_loop);
    g_signal_handler_disconnect (auth, handler);
    g_main_loop_unref (token_loop);
  }

  if (native_auth_has_valid_token (auth)) {
    g_clear_pointer (&self->login_token, g_free);
    self->login_token = g_strdup (native_auth_get_token (auth));
  }
  g_object_unref (auth);

  return self->login_token != NULL;
}

static gpointer
session_thread (gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  g_main_context_push_thread_default (self->context);

  if (!acquire_login_token (self)) {
    fail_session (self, "could not obtain an access token for AP login");
    g_main_context_pop_thread_default (self->context);
    return NULL;
  }

  g_main_context_invoke (self->context, begin_signin, self);

  /* Publish the loop under the lock and re-check `stopping` in the same
   * critical section. Without this, dispose() can run between "loop is
   * still NULL, nothing to quit" and this thread entering g_main_loop_run,
   * and then g_thread_join blocks forever on a loop nobody will ever quit. */
  g_mutex_lock (&self->lock);
  if (self->stopping) {
    g_mutex_unlock (&self->lock);
    g_main_context_pop_thread_default (self->context);
    return NULL;
  }
  self->loop = g_main_loop_new (self->context, FALSE);
  GMainLoop *loop = g_main_loop_ref (self->loop);
  g_mutex_unlock (&self->lock);

  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  g_main_context_pop_thread_default (self->context);
  return NULL;
}

/*
 * Re-sign-in after a drop, with a token good at the time of use.
 *
 * Not begin_signin() directly. That reuses login_token, which was fetched when
 * the session started and is good for an hour; a session outlives that easily,
 * so a reconnect an hour in presented an expired token and the AP answered
 * BadCredentials -- which surfaced as being signed out at random, triggered by
 * whatever first needed Mercury.
 */
static gboolean
resignin (gpointer user_data)
{
  SpotifyNativeSession *self = user_data;

  if (!acquire_login_token (self)) {
    fail_session (self, "could not refresh the access token to reconnect");
    return G_SOURCE_REMOVE;
  }
  return begin_signin (self);
}

/*
 * Tear the connection down and start it again.
 *
 * The session had no path back from a dropped AP link: start() returns early
 * once a thread exists, so calling it again did nothing. Mercury is dropped
 * with the connection because it rides on it and would otherwise keep sending
 * into a closed socket.
 */
void
spotifygtk_native_session_reconnect (SpotifyNativeSession *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));

  g_clear_object (&self->mercury);
  g_clear_object (&self->ap);

  /* Anything queued behind a load that will never finish. */
  collection_drain_waiters (self, NULL, "the connection was lost");

  if (self->context)
    g_main_context_invoke (self->context, resignin, self);
}


void
spotifygtk_native_session_report_playback (SpotifyNativeSession *self,
                                           const gchar *track_uri,
                                           gint64 position_ms,
                                           gboolean playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));

  g_free (connect_now_uri);
  connect_now_uri = g_strdup (track_uri);
  connect_now_position_ms = position_ms;
  connect_now_playing = playing;

  /* Tell the server now rather than at the next keepalive: a controller that
   * just handed playback over is waiting to see it start. */
  if (dealer_bearer && dealer_conn_id)
    connect_put_state (dealer_bearer, dealer_conn_id);
}

void
spotifygtk_native_session_start (SpotifyNativeSession *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));

  if (self->thread)
    return;   /* already connecting or ready */

  set_state (self, SPOTIFYGTK_SESSION_CONNECTING, "Connecting to Spotify…");

  self->context = g_main_context_new ();
  self->thread  = g_thread_new ("spotify-session", session_thread, self);
}

/* ── Catalog: context resolve + batched metadata ─────────────────────────── */

typedef struct {
  SpotifyNativeSession *session;   /* not owned; the GTask holds the ref */
  GTask                *task;
  gchar                *context_uri;
  guint                 max_tracks;

  /* Paging state. A context resolve hands back every track URI at once, but a
   * metadata request for all of them is refused outright once the list is big
   * enough (4773 rejected, 2000 accepted). So the URIs are kept here and walked
   * in SPOTIFYGTK_SESSION_MAX_BATCH-sized requests, accumulating into `out`. */
  GPtrArray            *uris;      /* gchar*, owned; the full list */
  guint                 next_uri;  /* start index of the request in flight */
  GPtrArray            *out;       /* SpotifyNativeTrack*, owned; accumulated */
} LoadTracksOp;

static void
load_op_free (LoadTracksOp *op)
{
  g_free (op->context_uri);
  g_clear_pointer (&op->uris, g_ptr_array_unref);
  g_clear_pointer (&op->out, g_ptr_array_unref);
  g_free (op);
}

/*
 * Finish everyone who joined the in-flight collection load, and clear the
 * marker that made them join.
 *
 * Every completion path for a collection load has to come through here. The
 * empty-context path did not, so a collection that resolved to zero tracks
 * left collection_inflight_uri set with nobody left to clear it: from then on
 * every load of that URI joined a queue that could never be satisfied, the
 * tasks piled up unreturned, and the page they belonged to waited forever.
 *
 * `tracks` NULL means failure and `message` says why; otherwise each waiter
 * gets its own copy, since callers own what they receive and some sort it in
 * place.
 */
static void
collection_drain_waiters (SpotifyNativeSession *self, GPtrArray *tracks,
                          const gchar *message)
{
  g_clear_pointer (&self->collection_inflight_uri, g_free);

  GList *waiters = self->collection_waiters;
  self->collection_waiters = NULL;
  if (!waiters)
    return;

  for (GList *l = waiters; l; l = l->next) {
    GTask *waiting = l->data;
    if (tracks) {
      GPtrArray *copy = g_ptr_array_new_with_free_func (
        (GDestroyNotify) spotifygtk_native_track_free);
      for (guint i = 0; i < tracks->len; i++)
        g_ptr_array_add (copy, spotifygtk_native_track_copy (
          g_ptr_array_index (tracks, i)));
      g_task_return_pointer (waiting, copy, (GDestroyNotify) g_ptr_array_unref);
    } else {
      g_task_return_new_error (waiting, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                               message ? message : "collection load failed");
    }
    g_object_unref (waiting);
  }

  g_message ("session: satisfied %u joined collection request(s) from the "
             "same load", g_list_length (waiters));
  g_list_free (waiters);
}

/* Hand the accumulated tracks back and finish the task. */
static void
load_op_finish (LoadTracksOp *op)
{
  GPtrArray *out = g_steal_pointer (&op->out);

  /* Keep collection loads for the other pages that will ask for the same URI.
   * Only the collection: everything else is per-view. */
  if (out && op->context_uri && strstr (op->context_uri, ":collection")) {
    SpotifyNativeSession *self = op->session;
    g_clear_pointer (&self->collection_cache, g_ptr_array_unref);
    g_free (self->collection_cache_uri);

    self->collection_cache_uri = g_strdup (op->context_uri);
    self->collection_cache = g_ptr_array_new_with_free_func (
      (GDestroyNotify) spotifygtk_native_track_free);
    for (guint i = 0; i < out->len; i++)
      g_ptr_array_add (self->collection_cache,
                       spotifygtk_native_track_copy (g_ptr_array_index (out, i)));

  }

  /*
   * Outside the cache block on purpose: a collection load that returned
   * nothing still has to release whoever joined it. Guarded on the URI rather
   * than on `out`, which is what decides whether anyone could have joined in
   * the first place.
   */
  if (op->context_uri && strstr (op->context_uri, ":collection"))
    collection_drain_waiters (op->session, out, NULL);

  g_task_return_pointer (op->task, out, (GDestroyNotify) g_ptr_array_unref);
  g_object_unref (op->task);
  load_op_free (op);
}

static void request_next_page (LoadTracksOp *op);

static void
on_batch_metadata (const SpclientTrackInfo *tracks, guint n_tracks,
                   GError *error, gpointer user_data)
{
  LoadTracksOp *op = user_data;

  if (error || !tracks) {
    /* A page that fails after earlier ones succeeded still leaves something
     * worth showing, and a partial collection beats an error page -- so only
     * fail the whole load when nothing at all was gathered. */
    if (op->out && op->out->len > 0) {
      g_warning ("session: metadata page starting at %u failed (%s); returning "
                 "the %u track(s) already loaded",
                 op->next_uri, error ? error->message : "no tracks returned",
                 op->out->len);
      load_op_finish (op);
      return;
    }
    /* Fail the joined requests too, or they would wait for a completion that
     * is never coming. */
    g_autofree gchar *why = g_strdup_printf ("could not load track metadata: %s",
      error ? error->message : "no tracks returned");
    collection_drain_waiters (op->session, NULL, why);

    g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_FAILED,
      "could not load track metadata: %s",
      error ? error->message : "no tracks returned");
    g_object_unref (op->task);
    load_op_free (op);
    return;
  }

  GPtrArray *out = op->out;

  /*
   * Emit in the order the URIs were *requested*, not the order they came back.
   *
   * The server is explicitly allowed to reorder or omit entries -- which is why
   * SpclientTrackInfo carries entity_uri rather than relying on position -- and
   * appending in response order silently threw the context's own ordering away.
   * On an album that is not cosmetic: the running order is part of the record,
   * and artists who cross-fade between tracks (Hybrid's "Disappear Here", say)
   * had their sequencing scrambled.
   *
   * Index this page by URI, then walk the request slice and place each track
   * where it belongs. A URI the server did not return is simply skipped.
   */
  g_autoptr(GHashTable) by_uri = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < n_tracks; i++) {
    if (tracks[i].entity_uri)
      g_hash_table_insert (by_uri, tracks[i].entity_uri, (gpointer) &tracks[i]);
  }

  guint page_len = MIN (op->uris->len - op->next_uri, SPOTIFYGTK_SESSION_MAX_BATCH);
  for (guint i = 0; i < page_len; i++) {
    const gchar *want = g_ptr_array_index (op->uris, op->next_uri + i);
    const SpclientTrackInfo *info = g_hash_table_lookup (by_uri, want);
    if (!info)
      continue;   /* server omitted this one; leave a gap rather than a hole */

    SpotifyNativeTrack *track = g_new0 (SpotifyNativeTrack, 1);

    track->uri         = g_strdup (info->entity_uri);
    track->name        = g_strdup (info->meta.name);
    track->artists     = g_strdup (info->meta.artist_names);
    track->album       = g_strdup (info->meta.album_name);
    track->duration_ms = info->meta.duration_ms;
    track->is_explicit = info->meta.is_explicit;
    track->cover_id    = g_strdup (info->meta.cover_id);
    track->cover_id_small = g_strdup (info->meta.cover_id_small);
    track->album_uri   = g_strdup (info->meta.album_uri);
    track->artist_uri  = g_strdup (info->meta.artist_uri);
    track->release_year = info->meta.release_year;

    g_ptr_array_add (out, track);
  }

  /* Advance by the page asked for, not by what came back -- otherwise an
   * omitted track would shift every later page and lose the tail. */
  op->next_uri += page_len;
  request_next_page (op);
}

/*
 * Request metadata for the next chunk, or finish if the list is exhausted.
 *
 * Sequential rather than parallel on purpose: the requests share one bearer and
 * one spclient, and a collection is three or four chunks at most, so overlapping
 * them buys little and makes cancellation and partial failure much harder to
 * reason about.
 */
static void
request_next_page (LoadTracksOp *op)
{
  if (!op->uris || op->next_uri >= op->uris->len) {
    load_op_finish (op);
    return;
  }

  guint remaining = op->uris->len - op->next_uri;
  guint n         = MIN (remaining, SPOTIFYGTK_SESSION_MAX_BATCH);

  g_mutex_lock (&op->session->lock);
  g_autofree gchar *bearer = g_strdup (op->session->bearer_token);
  g_autofree gchar *ctoken = g_strdup (op->session->client_token);
  g_mutex_unlock (&op->session->lock);

  if (op->uris->len > SPOTIFYGTK_SESSION_MAX_BATCH)
    g_message ("session: metadata page %u-%u of %u",
               op->next_uri, op->next_uri + n, op->uris->len);

  /* The chunk is a pointer slice into the existing array -- the URI strings are
   * borrowed for the duration of the call, not copied. */
  spotifygtk_spclient_get_tracks_metadata (op->session->spclient,
                                           (const gchar *const *) &op->uris->pdata[op->next_uri],
                                           n, bearer, ctoken,
                                           on_batch_metadata, op);
}

static void
on_context_resolved (JsonNode *context, GError *error, gpointer user_data)
{
  LoadTracksOp *op = user_data;

  if (error || !context) {
    g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_FAILED,
      "could not resolve context: %s",
      error ? error->message : "no data returned");
    g_object_unref (op->task);
    load_op_free (op);
    return;
  }

  /* Context → flat list of track URIs. ContextTrack carries only `uri`,
   * so this is all there is to extract; display metadata is the second
   * request below. */
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
          JsonObject *entry = json_array_get_object_element (tracks, t);
          if (!entry || !json_object_has_member (entry, "uri"))
            continue;
          if (uris->len >= op->max_tracks)
            break;
          g_ptr_array_add (uris, g_strdup (json_object_get_string_member (entry, "uri")));
        }
        if (uris->len >= op->max_tracks)
          break;
      }
    }
  }

  json_node_unref (context);

  if (uris->len == 0) {
    /* An empty context is a legitimate answer (no search results, empty
     * playlist), so return an empty array rather than an error and let the
     * UI say "nothing here". */
    g_ptr_array_unref (uris);
    GPtrArray *empty = g_ptr_array_new_with_free_func (
      (GDestroyNotify) spotifygtk_native_track_free);

    /* An empty collection is still a completed collection load. Skipping this
     * is what wedged the marker and stranded every later request. */
    if (op->context_uri && strstr (op->context_uri, ":collection"))
      collection_drain_waiters (op->session, empty, NULL);

    g_task_return_pointer (op->task, empty, (GDestroyNotify) g_ptr_array_unref);
    g_object_unref (op->task);
    load_op_free (op);
    return;
  }

  /* Hand the list to the op and walk it a page at a time. No NULL terminator:
   * get_tracks_metadata() takes an explicit count, which is what lets a page be
   * a plain pointer slice into this array rather than a copy. */
  op->uris     = uris;
  op->next_uri = 0;
  op->out      = g_ptr_array_new_with_free_func (
                   (GDestroyNotify) spotifygtk_native_track_free);

  request_next_page (op);
}

/* Issue the context-resolve for an op whose bearer is known good. */
static void
run_load_op (LoadTracksOp *op)
{
  SpotifyNativeSession *self = op->session;

  g_mutex_lock (&self->lock);
  g_autofree gchar *bearer = g_strdup (self->bearer_token);
  g_autofree gchar *ctoken = g_strdup (self->client_token);
  g_mutex_unlock (&self->lock);

  spotifygtk_spclient_get_context (self->spclient, op->context_uri,
                                   bearer, ctoken,
                                   on_context_resolved, op);
}

/* Called when a refresh settles: run everything that was waiting, or fail it
 * all if the refresh could not produce a usable bearer. */
static void
flush_pending_ops (SpotifyNativeSession *self, gboolean refresh_ok)
{
  GList *ops = self->pending_ops;
  self->pending_ops = NULL;

  for (GList *l = ops; l; l = l->next) {
    LoadTracksOp *op = l->data;

    if (refresh_ok) {
      run_load_op (op);
    } else {
      g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "session credentials expired and could not be renewed");
      g_object_unref (op->task);
      load_op_free (op);
    }
  }

  g_list_free (ops);
}

static gboolean
start_load_tracks (gpointer user_data)
{
  LoadTracksOp *op = user_data;
  SpotifyNativeSession *self = op->session;

  g_mutex_lock (&self->lock);
  gboolean ready = (self->state == SPOTIFYGTK_SESSION_READY);
  gint64   expires_at = self->bearer_expires_at;
  g_mutex_unlock (&self->lock);

  if (!ready || !self->spclient) {
    g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                             "session is not signed in yet");
    g_object_unref (op->task);
    load_op_free (op);
    return G_SOURCE_REMOVE;
  }

  /* Re-mint before use rather than after a 401: the failure mode otherwise
   * is a session that still says READY while every request is rejected. */
  gboolean stale = expires_at > 0 &&
                   g_get_monotonic_time () > expires_at - BEARER_REFRESH_MARGIN_US;

  if (stale || self->refreshing) {
    self->pending_ops = g_list_append (self->pending_ops, op);
    if (!self->refreshing)
      start_bearer_refresh (self);
    return G_SOURCE_REMOVE;
  }

  run_load_op (op);
  return G_SOURCE_REMOVE;
}

void
spotifygtk_native_session_load_tracks (SpotifyNativeSession *self,
                                       const gchar          *context_uri,
                                       guint                 max_tracks,
                                       GCancellable         *cancellable,
                                       GAsyncReadyCallback   callback,
                                       gpointer              user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));
  g_return_if_fail (context_uri != NULL);

  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, spotifygtk_native_session_load_tracks);

  if (max_tracks == 0)
    max_tracks = SPOTIFYGTK_SESSION_DEFAULT_MAX_TRACKS;
  /* Clamped to the overall ceiling, not to one request's worth: anything larger
   * than a single batch is paged rather than truncated. */
  max_tracks = MIN (max_tracks, SPOTIFYGTK_SESSION_MAX_TRACKS);

  if (!self->context) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                             "session has not been started");
    g_object_unref (task);
    return;
  }

  /* Already fetching this exact URI: wait for it rather than duplicating it. */
  if (self->collection_inflight_uri &&
      g_strcmp0 (context_uri, self->collection_inflight_uri) == 0) {
    g_message ("session: joining the collection load already in flight");
    self->collection_waiters = g_list_append (self->collection_waiters, task);
    return;   /* the task ref transfers to the waiter list */
  }

  /* Cache hit: hand back a copy and skip the network entirely. A copy rather
   * than the array itself because callers own what they receive and some sort
   * it in place. */
  if (self->collection_cache && self->collection_cache_uri &&
      g_strcmp0 (context_uri, self->collection_cache_uri) == 0) {
    GPtrArray *copy = g_ptr_array_new_with_free_func (
      (GDestroyNotify) spotifygtk_native_track_free);
    for (guint i = 0; i < self->collection_cache->len; i++)
      g_ptr_array_add (copy, spotifygtk_native_track_copy (
        g_ptr_array_index (self->collection_cache, i)));

    g_message ("session: serving %u collection track(s) from cache", copy->len);
    g_task_return_pointer (task, copy, (GDestroyNotify) g_ptr_array_unref);
    g_object_unref (task);
    return;
  }

  if (strstr (context_uri, ":collection")) {
    g_free (self->collection_inflight_uri);
    self->collection_inflight_uri = g_strdup (context_uri);
  }

  LoadTracksOp *op = g_new0 (LoadTracksOp, 1);
  op->session     = self;
  op->task        = task;   /* ref transferred; holds a ref on `self` too */
  op->context_uri = g_strdup (context_uri);
  op->max_tracks  = max_tracks;

  /* Hop to the worker: every spclient call must run on its context. */
  g_main_context_invoke (self->context, start_load_tracks, op);
}


/* ── Discography ─────────────────────────────────────────────────────────── */

#define DISCO_ALBUM_BATCH 300   /* well under the batch ceiling probed for tracks */

void
spotifygtk_native_release_free (SpotifyNativeRelease *release)
{
  if (!release)
    return;
  g_free (release->uri);
  g_free (release->name);
  g_free (release->cover_id);
  g_clear_pointer (&release->tracks, g_ptr_array_unref);
  g_free (release);
}

typedef struct {
  SpotifyNativeSession *session;
  GTask                *task;
  gchar                *artist_uri;

  GPtrArray            *releases;    /* SpotifyNativeRelease*, the result */
  GHashTable           *by_uri;      /* album uri -> SpotifyNativeRelease* (borrowed) */

  GPtrArray            *album_uris;  /* gchar* */
  guint                 next_album;

  GPtrArray            *track_uris;  /* gchar* */
  GHashTable           *track_owner; /* track uri -> SpotifyNativeRelease* (borrowed) */
  guint                 next_track;
} DiscoOp;

static void disco_request_albums (DiscoOp *op);
static void disco_request_tracks (DiscoOp *op);

static void
disco_op_free (DiscoOp *op)
{
  if (!op)
    return;
  g_free (op->artist_uri);
  g_clear_pointer (&op->releases, g_ptr_array_unref);
  g_clear_pointer (&op->by_uri, g_hash_table_unref);
  g_clear_pointer (&op->album_uris, g_ptr_array_unref);
  g_clear_pointer (&op->track_uris, g_ptr_array_unref);
  g_clear_pointer (&op->track_owner, g_hash_table_unref);
  g_free (op);
}

static void
disco_fail (DiscoOp *op, const gchar *what, GError *error)
{
  g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "could not load the discography (%s): %s", what,
                           error ? error->message : "no data returned");
  g_object_unref (op->task);
  disco_op_free (op);
}

static void
disco_finish (DiscoOp *op)
{
  g_task_return_pointer (op->task, g_steal_pointer (&op->releases),
                         (GDestroyNotify) g_ptr_array_unref);
  g_object_unref (op->task);
  disco_op_free (op);
}

/* Third leg: names and durations for the track URIs the albums listed. */
static void
on_disco_tracks (const SpclientTrackInfo *tracks, guint n_tracks,
                 GError *error, gpointer user_data)
{
  DiscoOp *op = user_data;

  if (error) {
    disco_fail (op, "track metadata", error);
    return;
  }

  g_autoptr(GHashTable) by_uri = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < n_tracks; i++) {
    if (tracks[i].entity_uri)
      g_hash_table_insert (by_uri, tracks[i].entity_uri, (gpointer) &tracks[i]);
  }

  /* Walk the slice that was *requested*, so each release keeps its running
   * order -- the same reason the context loader does it this way. */
  guint page = MIN (op->track_uris->len - op->next_track, SPOTIFYGTK_SESSION_MAX_BATCH);
  for (guint i = 0; i < page; i++) {
    const gchar *want = g_ptr_array_index (op->track_uris, op->next_track + i);
    const SpclientTrackInfo *info = g_hash_table_lookup (by_uri, want);
    SpotifyNativeRelease *owner = g_hash_table_lookup (op->track_owner, want);
    if (!info || !owner)
      continue;

    SpotifyNativeTrack *t = g_new0 (SpotifyNativeTrack, 1);
    t->uri            = g_strdup (info->entity_uri);
    t->name           = g_strdup (info->meta.name);
    t->artists        = g_strdup (info->meta.artist_names);
    t->album          = g_strdup (info->meta.album_name ? info->meta.album_name : owner->name);
    t->duration_ms    = info->meta.duration_ms;
    t->is_explicit    = info->meta.is_explicit;
    t->cover_id       = g_strdup (info->meta.cover_id ? info->meta.cover_id : owner->cover_id);
    t->cover_id_small = g_strdup (info->meta.cover_id_small);
    t->album_uri      = g_strdup (info->meta.album_uri ? info->meta.album_uri : owner->uri);
    t->artist_uri     = g_strdup (info->meta.artist_uri);
    t->release_year   = info->meta.release_year ? info->meta.release_year : owner->year;

    g_ptr_array_add (owner->tracks, t);
  }

  op->next_track += page;
  disco_request_tracks (op);
}

static void
disco_request_tracks (DiscoOp *op)
{
  if (!op->track_uris || op->next_track >= op->track_uris->len) {
    disco_finish (op);
    return;
  }

  guint n = MIN (op->track_uris->len - op->next_track, SPOTIFYGTK_SESSION_MAX_BATCH);

  g_mutex_lock (&op->session->lock);
  g_autofree gchar *bearer = g_strdup (op->session->bearer_token);
  g_autofree gchar *ctoken = g_strdup (op->session->client_token);
  g_mutex_unlock (&op->session->lock);

  spotifygtk_spclient_get_tracks_metadata (op->session->spclient,
                                           (const gchar *const *) &op->track_uris->pdata[op->next_track],
                                           n, bearer, ctoken, on_disco_tracks, op);
}

/* Second leg: name, year, type, cover and track URIs per release. */
static void
on_disco_albums (GPtrArray *albums, GError *error, gpointer user_data)
{
  DiscoOp *op = user_data;

  if (error) {
    disco_fail (op, "album metadata", error);
    return;
  }

  for (guint i = 0; albums && i < albums->len; i++) {
    const SpotifyRelease *a = g_ptr_array_index (albums, i);
    SpotifyNativeRelease *r = a->uri ? g_hash_table_lookup (op->by_uri, a->uri) : NULL;
    if (!r)
      continue;

    if (!r->name && a->name)
      r->name = g_strdup (a->name);
    if (a->year)
      r->year = a->year;
    if (a->type)
      r->type = a->type;
    if (!r->cover_id && a->cover_id)
      r->cover_id = g_strdup (a->cover_id);

    for (guint j = 0; a->track_uris && j < a->track_uris->len; j++) {
      const gchar *turi = g_ptr_array_index (a->track_uris, j);
      if (!turi || g_hash_table_contains (op->track_owner, turi))
        continue;   /* the same recording can appear on more than one release */
      gchar *owned = g_strdup (turi);
      g_ptr_array_add (op->track_uris, owned);
      g_hash_table_insert (op->track_owner, owned, r);
    }
  }

  op->next_album += MIN (op->album_uris->len - op->next_album, DISCO_ALBUM_BATCH);
  disco_request_albums (op);
}

static void
disco_request_albums (DiscoOp *op)
{
  if (!op->album_uris || op->next_album >= op->album_uris->len) {
    disco_request_tracks (op);
    return;
  }

  guint n = MIN (op->album_uris->len - op->next_album, DISCO_ALBUM_BATCH);

  g_mutex_lock (&op->session->lock);
  g_autofree gchar *bearer = g_strdup (op->session->bearer_token);
  g_autofree gchar *ctoken = g_strdup (op->session->client_token);
  g_mutex_unlock (&op->session->lock);

  spotifygtk_spclient_get_albums_metadata (op->session->spclient,
                                           (const gchar *const *) &op->album_uris->pdata[op->next_album],
                                           n, bearer, ctoken, on_disco_albums, op);
}

/*
 * Album metadata for a known list of album URIs.
 *
 * The middle leg of the discography load on its own, for callers that already
 * know which albums they want -- the library's saved albums, which come out of
 * the collection rather than out of an artist.
 */
typedef struct {
  SpotifyNativeSession *session;
  GTask                *task;
  GPtrArray            *uris;      /* gchar* */
} AlbumsOp;

static void
albums_op_free (AlbumsOp *op)
{
  g_clear_pointer (&op->uris, g_ptr_array_unref);
  g_free (op);
}

static void
on_albums_meta (GPtrArray *albums, GError *error, gpointer user_data)
{
  AlbumsOp *op = user_data;

  if (error) {
    g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "could not load album metadata: %s", error->message);
    g_object_unref (op->task);
    albums_op_free (op);
    return;
  }

  GPtrArray *out = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_release_free);

  for (guint i = 0; albums && i < albums->len; i++) {
    const SpotifyRelease *a = g_ptr_array_index (albums, i);
    if (!a->uri)
      continue;
    SpotifyNativeRelease *r = g_new0 (SpotifyNativeRelease, 1);
    r->uri      = g_strdup (a->uri);
    r->name     = g_strdup (a->name);
    r->year     = a->year;
    r->type     = a->type;
    r->cover_id = g_strdup (a->cover_id);
    r->tracks   = g_ptr_array_new_with_free_func (
      (GDestroyNotify) spotifygtk_native_track_free);
    g_ptr_array_add (out, r);
  }

  g_task_return_pointer (op->task, out, (GDestroyNotify) g_ptr_array_unref);
  g_object_unref (op->task);
  albums_op_free (op);
}

static gboolean
start_load_albums (gpointer user_data)
{
  AlbumsOp *op = user_data;

  g_mutex_lock (&op->session->lock);
  g_autofree gchar *bearer = g_strdup (op->session->bearer_token);
  g_autofree gchar *ctoken = g_strdup (op->session->client_token);
  g_mutex_unlock (&op->session->lock);

  if (!op->session->spclient || op->uris->len == 0) {
    g_task_return_pointer (op->task,
                           g_ptr_array_new_with_free_func (
                             (GDestroyNotify) spotifygtk_native_release_free),
                           (GDestroyNotify) g_ptr_array_unref);
    g_object_unref (op->task);
    albums_op_free (op);
    return G_SOURCE_REMOVE;
  }

  spotifygtk_spclient_get_albums_metadata (op->session->spclient,
                                           (const gchar *const *) op->uris->pdata,
                                           op->uris->len, bearer, ctoken,
                                           on_albums_meta, op);
  return G_SOURCE_REMOVE;
}

void
spotifygtk_native_session_load_albums (SpotifyNativeSession *self,
                                       const gchar *const   *uris,
                                       guint                 n_uris,
                                       GCancellable         *cancellable,
                                       GAsyncReadyCallback   callback,
                                       gpointer              user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));

  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, spotifygtk_native_session_load_albums);

  if (!self->context) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                             "session has not been started");
    g_object_unref (task);
    return;
  }

  AlbumsOp *op = g_new0 (AlbumsOp, 1);
  op->session = self;
  op->task    = task;
  op->uris    = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < n_uris; i++)
    if (uris[i]) g_ptr_array_add (op->uris, g_strdup (uris[i]));

  g_main_context_invoke (self->context, start_load_albums, op);
}

GPtrArray *
spotifygtk_native_session_load_albums_finish (SpotifyNativeSession *self,
                                              GAsyncResult         *result,
                                              GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* First leg: every release URI the artist has, and which group it sits in. */
static void
on_disco_releases (GPtrArray *releases, GError *error, gpointer user_data)
{
  DiscoOp *op = user_data;

  if (error || !releases) {
    disco_fail (op, "release list", error);
    return;
  }

  for (guint i = 0; i < releases->len; i++) {
    const SpotifyRelease *src = g_ptr_array_index (releases, i);
    if (!src->uri || g_hash_table_contains (op->by_uri, src->uri))
      continue;

    SpotifyNativeRelease *r = g_new0 (SpotifyNativeRelease, 1);
    r->uri    = g_strdup (src->uri);
    r->group  = src->group;
    r->tracks = g_ptr_array_new_with_free_func (
      (GDestroyNotify) spotifygtk_native_track_free);

    g_ptr_array_add (op->releases, r);
    g_hash_table_insert (op->by_uri, r->uri, r);
    g_ptr_array_add (op->album_uris, r->uri);   /* borrowed; the release owns it */
  }

  g_message ("session: %u releases for %s", op->releases->len, op->artist_uri);
  disco_request_albums (op);
}

static gboolean
start_discography (gpointer user_data)
{
  DiscoOp *op = user_data;

  if (!op->session->spclient) {
    disco_fail (op, "session", NULL);
    return G_SOURCE_REMOVE;
  }

  g_mutex_lock (&op->session->lock);
  g_autofree gchar *bearer = g_strdup (op->session->bearer_token);
  g_autofree gchar *ctoken = g_strdup (op->session->client_token);
  g_mutex_unlock (&op->session->lock);

  spotifygtk_spclient_get_artist_releases (op->session->spclient, op->artist_uri,
                                           bearer, ctoken, on_disco_releases, op);
  return G_SOURCE_REMOVE;
}

void
spotifygtk_native_session_load_discography (SpotifyNativeSession *self,
                                            const gchar          *artist_uri,
                                            GCancellable         *cancellable,
                                            GAsyncReadyCallback   callback,
                                            gpointer              user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));
  g_return_if_fail (artist_uri != NULL);

  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, spotifygtk_native_session_load_discography);

  if (!self->context) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                             "session has not been started");
    g_object_unref (task);
    return;
  }

  DiscoOp *op = g_new0 (DiscoOp, 1);
  op->session     = self;
  op->task        = task;
  op->artist_uri  = g_strdup (artist_uri);
  op->releases    = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_release_free);
  op->by_uri      = g_hash_table_new (g_str_hash, g_str_equal);
  op->album_uris  = g_ptr_array_new ();               /* borrowed strings */
  op->track_uris  = g_ptr_array_new_with_free_func (g_free);
  op->track_owner = g_hash_table_new (g_str_hash, g_str_equal);

  g_main_context_invoke (self->context, start_discography, op);
}

GPtrArray *
spotifygtk_native_session_load_discography_finish (SpotifyNativeSession *self,
                                                   GAsyncResult         *result,
                                                   GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}


typedef struct {
  SpotifyNativeSession        *session;
  gchar                       *uri;
  SpotifyNativeArtistImageFunc callback;
  gpointer                     user_data;
} ArtistImageOp;

/*
 * artist uri -> banner image id, for the life of the session.
 *
 * Measured cold: 858 ms to learn the id, 240 ms to fetch and decode it. All
 * of that first number is one pathfinder round trip returning the entire
 * artist overview -- discography, top tracks, related artists -- to extract a
 * single string. The query is persisted, so it cannot be narrowed. What can be
 * avoided is asking twice for an artist already visited.
 */
static GHashTable *artist_image_cache;   /* owned strings both sides */

/*
 * ...and on disk, so a restart does not pay for it again.
 *
 * The images themselves are already cached as files, keyed by id -- but the id
 * is what costs the round trip, so caching the art without caching the lookup
 * still meant 858 ms before a banner could even be asked for. A few hundred
 * artists is a few tens of KB, next to a cache budget measured in gigabytes.
 *
 * Stale entries are harmless: an artist who changes their banner gets the old
 * id, which either still resolves or 404s and falls back, and either way the
 * next fetch of the overview corrects it.
 */
static gchar *
artist_image_map_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), "spotifygtk",
                           "artist-images", NULL);
}

static void
artist_image_map_load (void)
{
  if (artist_image_cache)
    return;
  artist_image_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, g_free);

  g_autofree gchar *path = artist_image_map_path ();
  g_autofree gchar *data = NULL;
  if (!path || !g_file_get_contents (path, &data, NULL, NULL))
    return;

  g_auto(GStrv) lines = g_strsplit (data, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    if (!*lines[i])
      continue;
    gchar *sp = strchr (lines[i], ' ');
    if (!sp)
      continue;
    *sp = '\0';
    g_hash_table_insert (artist_image_cache, g_strdup (lines[i]),
                         g_strdup (sp + 1));
  }
}

static void
artist_image_map_save (void)
{
  if (!artist_image_cache)
    return;

  g_autofree gchar *path = artist_image_map_path ();
  if (!path)
    return;
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);

  GString *out = g_string_new (NULL);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, artist_image_cache);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_string_append_printf (out, "%s %s\n", (const gchar *) k, (const gchar *) v);

  /* Nothing to handle on failure: the map is an optimisation, and an unwritten
   * one costs exactly the round trip it was there to save. */
  g_file_set_contents (path, out->str, (gssize) out->len, NULL);
  g_string_free (out, TRUE);
}

static void
artist_image_done (ArtistImageOp *op, const gchar *cover_id)
{
  if (cover_id && *cover_id && op->uri) {
    artist_image_map_load ();
    const gchar *known = g_hash_table_lookup (artist_image_cache, op->uri);
    if (g_strcmp0 (known, cover_id) != 0) {
      g_hash_table_insert (artist_image_cache, g_strdup (op->uri),
                           g_strdup (cover_id));
      artist_image_map_save ();
    }
  }
  if (op->callback)
    op->callback (cover_id, op->user_data);
  g_free (op->uri);
  g_free (op);
}

/* The avatar, asked for only when there is no header to show. */
static void
on_artist_portrait (const gchar *cover_id, GError *error, gpointer user_data)
{
  ArtistImageOp *op = user_data;
  if (error)
    g_message ("session: no artist portrait for %s (%s)", op->uri, error->message);
  artist_image_done (op, cover_id);
}

/*
 * The banner first, the avatar only if there is none.
 *
 * These are two different images and the difference is the whole point:
 * `headerImage` is the wide backdrop the artist uploads, `portrait_group` is
 * the round profile picture. Covering a panel with the second is what made
 * every artist page look broken. Plenty of artists have published no banner,
 * which is not an error -- hence the fall back, and hence the log line, since
 * a silent fallback here is exactly what hid a broken request for so long.
 */
static void
on_artist_header (const gchar *cover_id, GError *error, gpointer user_data)
{
  ArtistImageOp *op = user_data;

  if (cover_id && *cover_id) {
    artist_image_done (op, cover_id);
    return;
  }

  g_message ("session: no artist banner for %s (%s); using the avatar",
             op->uri, error ? error->message : "none published");

  SpotifyNativeSession *self = op->session;
  g_mutex_lock (&self->lock);
  g_autofree gchar *bearer = g_strdup (self->bearer_token);
  g_autofree gchar *ctoken = g_strdup (self->client_token);
  g_mutex_unlock (&self->lock);

  spotifygtk_spclient_get_artist_portrait (self->spclient, op->uri, bearer, ctoken,
                                           on_artist_portrait, op);
}

static gboolean
start_artist_image (gpointer user_data)
{
  ArtistImageOp *op = user_data;
  SpotifyNativeSession *self = op->session;

  /* Already known, from this session or an earlier one: no round trip. */
  artist_image_map_load ();
  const gchar *known = g_hash_table_lookup (artist_image_cache, op->uri);
  if (known) {
    artist_image_done (op, known);
    return G_SOURCE_REMOVE;
  }

  g_mutex_lock (&self->lock);
  g_autofree gchar *bearer = g_strdup (self->bearer_token);
  g_autofree gchar *ctoken = g_strdup (self->client_token);
  g_mutex_unlock (&self->lock);

  if (!self->spclient) {
    if (op->callback) op->callback (NULL, op->user_data);
    g_free (op->uri);
    g_free (op);
    return G_SOURCE_REMOVE;
  }

  spotifygtk_spclient_get_artist_header (self->spclient, op->uri, bearer, ctoken,
                                         on_artist_header, op);
  return G_SOURCE_REMOVE;
}

void
spotifygtk_native_session_get_artist_image (SpotifyNativeSession *self,
                                            const gchar          *artist_uri,
                                            SpotifyNativeArtistImageFunc callback,
                                            gpointer              user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));

  if (!artist_uri || !self->context) {
    if (callback) callback (NULL, user_data);
    return;
  }

  ArtistImageOp *op = g_new0 (ArtistImageOp, 1);
  op->session   = self;
  op->uri       = g_strdup (artist_uri);
  op->callback  = callback;
  op->user_data = user_data;

  /* Onto the worker: every spclient call must run on the session's context. */
  g_main_context_invoke (self->context, start_artist_image, op);
}

void
spotifygtk_native_session_invalidate_context (SpotifyNativeSession *self,
                                              const gchar          *context_uri)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self));

  if (!self->collection_cache_uri)
    return;
  if (context_uri && g_strcmp0 (context_uri, self->collection_cache_uri) != 0)
    return;

  g_message ("session: dropped the cached listing for %s",
             self->collection_cache_uri);
  g_clear_pointer (&self->collection_cache, g_ptr_array_unref);
  g_clear_pointer (&self->collection_cache_uri, g_free);
}

GPtrArray *
spotifygtk_native_session_load_tracks_finish (SpotifyNativeSession *self,
                                              GAsyncResult         *result,
                                              GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

SpotifyNativeSessionState
spotifygtk_native_session_get_state (SpotifyNativeSession *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self), SPOTIFYGTK_SESSION_FAILED);

  g_mutex_lock (&self->lock);
  SpotifyNativeSessionState state = self->state;
  g_mutex_unlock (&self->lock);
  return state;
}

gchar *
spotifygtk_native_session_dup_username (SpotifyNativeSession *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self), NULL);

  g_mutex_lock (&self->lock);
  gchar *username = g_strdup (self->username);
  g_mutex_unlock (&self->lock);
  return username;
}

SpotifyMercury *
spotifygtk_native_session_get_mercury (SpotifyNativeSession *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_NATIVE_SESSION (self), NULL);

  /*
   * Only once the AP handshake and login are through: Mercury rides that
   * connection and has nothing to send on before then.
   *
   * A dropped connection used to be permanent. Nothing here watched for one
   * and nothing reconnected, so once the AP went away this returned NULL for
   * the rest of the process and every write that depended on it failed
   * silently -- "cannot change Liked Songs: not signed in yet", repeatedly,
   * with no request ever leaving. Noticing it here is the earliest point that
   * anything asks.
   */
  if (self->ap && !spotifygtk_ap_session_is_live (self->ap) &&
      self->state == SPOTIFYGTK_SESSION_READY) {
    g_warning ("session: the AP connection is gone; reconnecting");
    set_state (self, SPOTIFYGTK_SESSION_CONNECTING, "Reconnecting…");
    spotifygtk_native_session_reconnect (self);
    return NULL;
  }

  if (!self->ap || !spotifygtk_ap_session_is_live (self->ap))
    return NULL;

  if (!self->mercury)
    self->mercury = spotifygtk_mercury_new (self->ap);
  return self->mercury;
}

gchar *
spotifygtk_native_session_dup_collection_uri (SpotifyNativeSession *self)
{
  g_autofree gchar *username = spotifygtk_native_session_dup_username (self);
  if (!username)
    return NULL;
  return spotifygtk_spclient_build_collection_uri (username);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

static void
spotifygtk_native_session_dispose (GObject *object)
{
  SpotifyNativeSession *self = SPOTIFYGTK_NATIVE_SESSION (object);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  /* Claim the loop under the lock so the worker cannot start one after this
   * point — see the matching comment in session_thread(). */
  g_mutex_lock (&self->lock);
  self->stopping = TRUE;
  GMainLoop *loop = self->loop ? g_main_loop_ref (self->loop) : NULL;
  g_mutex_unlock (&self->lock);

  if (loop) {
    g_main_loop_quit (loop);
    g_main_loop_unref (loop);
  }

  if (self->thread) {
    g_thread_join (self->thread);
    self->thread = NULL;
  }

  /* The worker has stopped, so anything still queued behind a refresh will
   * never run. Fail those tasks rather than leaking them and leaving their
   * callers waiting forever. */
  for (GList *l = self->pending_ops; l; l = l->next) {
    LoadTracksOp *op = l->data;
    g_task_return_new_error (op->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "session shut down");
    g_object_unref (op->task);
    load_op_free (op);
  }
  g_clear_pointer (&self->pending_ops, g_list_free);

  g_clear_pointer (&self->loop, g_main_loop_unref);
  g_clear_pointer (&self->context, g_main_context_unref);
  g_clear_pointer (&self->caller_context, g_main_context_unref);

  /* Anything still queued behind an in-flight load would otherwise never be
   * returned, and GTask warns loudly when one is finalized unreturned. */
  collection_drain_waiters (self, NULL, "the session was shut down");

  g_clear_object (&self->mercury);
  g_clear_object (&self->ap);
  g_clear_object (&self->client_token_client);
  g_clear_object (&self->login5_client);
  g_clear_object (&self->spclient);
  g_clear_object (&self->cancellable);

  g_clear_pointer (&self->device_id, g_free);
  g_clear_pointer (&self->login_token, g_free);
  g_clear_pointer (&self->bearer_token, g_free);
  g_clear_pointer (&self->client_token, g_free);
  g_clear_pointer (&self->username, g_free);

  G_OBJECT_CLASS (spotifygtk_native_session_parent_class)->dispose (object);
}

static void
spotifygtk_native_session_finalize (GObject *object)
{
  SpotifyNativeSession *self = SPOTIFYGTK_NATIVE_SESSION (object);
  g_clear_pointer (&self->collection_cache, g_ptr_array_unref);
  g_clear_pointer (&self->collection_cache_uri, g_free);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (spotifygtk_native_session_parent_class)->finalize (object);
}

static void
spotifygtk_native_session_class_init (SpotifyNativeSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose  = spotifygtk_native_session_dispose;
  object_class->finalize = spotifygtk_native_session_finalize;

  /*
   * A controller handed playback to this device. Carries what the transfer
   * asked for: the track, where to resume, and whether it was paused.
   *
   * A signal because starting playback is the window's job -- session.c speaks
   * the protocol and owns no player -- and because the transfer arrives on a
   * socket callback rather than anywhere the UI could reach.
   */
  signals[TRANSFER_REQUESTED] = g_signal_new ("transfer-requested",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_BOOLEAN);

  /* A controller pressed something: pause, resume, next, previous, seek. The
   * string is the endpoint as Spotify names it; the gint64 is the seek target
   * in milliseconds and is meaningless for the rest. */
  signals[REMOTE_COMMAND] = g_signal_new ("remote-command",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT64);

  signals[STATE_CHANGED] = g_signal_new ("state-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);
}

static void
spotifygtk_native_session_init (SpotifyNativeSession *self)
{
  g_mutex_init (&self->lock);
  self->state       = SPOTIFYGTK_SESSION_IDLE;
  self->cancellable = g_cancellable_new ();
  self->device_id   = generate_device_id ();

  self->caller_context = g_main_context_ref_thread_default ();
}

SpotifyNativeSession *
spotifygtk_native_session_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_NATIVE_SESSION, NULL);
}

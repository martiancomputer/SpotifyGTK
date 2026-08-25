/*
 * mercury.c — Mercury pub/sub implementation.
 *
 * The wire format below is no longer inferred. It is taken from librespot's
 * core/src/mercury (dispatch() and MercuryRequest::encode) and its
 * protocol/proto/mercury.proto, which is a working implementation against live
 * Spotify servers:
 *
 *     [u16 seq_len][seq_len bytes of seq][u8 flags][u16 part_count]
 *     then per part: [u16 length][data]
 *
 * Part 0 is always a protobuf `Header` -- uri = 1, content_type = 2,
 * method = 3, status_code = 4 (sint32, so zigzag) -- and the remaining parts
 * are the payload. `flags` is fragmentation, not the method: 1 means this
 * packet completes the message, 2 means its final part is a fragment that
 * continues in the next packet with the same sequence number.
 *
 * The previous encoder here was wrong in three ways that would each have been
 * enough on their own: it wrote the sequence number as a bare 8 bytes with no
 * length prefix, it put the raw URI string where the Header protobuf belongs,
 * and it passed the *method* as the flags byte. None of it had ever been sent,
 * because there was no receive half to notice.
 */

#include "config.h"
#include "mercury.h"
#include "protobuf_min.h"
#include <string.h>

/* Header field numbers, from mercury.proto. */
#define MERCURY_HDR_URI          1
#define MERCURY_HDR_CONTENT_TYPE 2
#define MERCURY_HDR_METHOD       3
#define MERCURY_HDR_STATUS_CODE  4
#define MERCURY_HDR_USER_FIELDS  6

/* UserField { key = 1 (string), value = 2 (bytes) } */
#define MERCURY_UF_KEY   1
#define MERCURY_UF_VALUE 2

/* flags byte */
#define MERCURY_FLAG_FINAL   1
#define MERCURY_FLAG_PARTIAL 2

struct _SpotifyMercury {
  GObject           parent_instance;
  SpotifyApSession *ap_session;

  /*
   * The AP session, Shannon send cipher, sequence counters and the two hash
   * tables below are one-thread objects.  Requests used to be issued directly
   * by GTK callbacks while replies arrived on the session worker, so both
   * threads modified pending/next_seq and encrypted packets concurrently.
   * Apart from corrupting Mercury state, response callbacks then ran on the
   * worker and called GTK there.
   *
   * Capture the context on which Mercury is constructed (the session worker)
   * and marshal every request to it.  Each request separately remembers the
   * caller's context so its completion returns to GTK, or remains on the
   * worker for protocol-internal callers.
   */
  GMainContext     *owner_context;
  GMutex            config_lock;
  gchar            *content_type;
  gint              callbacks_enabled;

  guint64           next_seq;
  GHashTable       *pending;        /* seq (guint64) -> MercuryPending* */
  GHashTable       *subscriptions;  /* uri (gchar*) -> Subscription* */
  guint64           next_sub_id;
};

typedef struct {
  guint64          id;
  gchar           *uri;
  MercuryCallback  callback;
  gpointer         user_data;
  GMainContext    *callback_context;
} Subscription;

/*
 * A message being assembled. Mercury may split one logical response across
 * several packets, so parts accumulate here until a packet arrives with the
 * FINAL flag.
 */
typedef struct {
  GPtrArray       *parts;      /* GBytes*, complete parts in order */
  GByteArray      *partial;    /* trailing fragment awaiting continuation */
  MercuryCallback  callback;
  gpointer         user_data;
  GMainContext    *callback_context;
} MercuryPending;

static void
mercury_pending_free (gpointer data)
{
  MercuryPending *p = data;
  if (!p) return;
  g_clear_pointer (&p->parts,   g_ptr_array_unref);
  g_clear_pointer (&p->partial, g_byte_array_unref);
  g_clear_pointer (&p->callback_context, g_main_context_unref);
  g_free (p);
}

typedef struct {
  SpotifyMercury *mercury;
  MercuryMethod   method;
  gchar          *method_override;
  gchar          *uri;
  GBytes         *payload;
  gchar         **field_keys;
  gchar         **field_values;
  guint           n_fields;
  GPtrArray      *parts;
  MercuryCallback callback;
  gpointer         user_data;
  GMainContext    *callback_context;
  gboolean         multipart;
} MercuryRequestDispatch;

typedef struct {
  SpotifyMercury  *mercury;
  MercuryCallback callback;
  gpointer        user_data;
  MercuryResponse *response;
} MercuryCallbackDispatch;

typedef struct {
  SpotifyMercury *mercury;
  Subscription   *subscription;
  guint64         unsubscribe_id;
} MercurySubscriptionDispatch;

static void
mercury_response_copy_free (MercuryResponse *response)
{
  if (!response)
    return;
  g_clear_pointer (&response->parts, g_ptr_array_unref);
  g_free (response->uri);
  g_free (response);
}

static gboolean
run_callback_dispatch (gpointer user_data)
{
  MercuryCallbackDispatch *dispatch = user_data;
  if (g_atomic_int_get (&dispatch->mercury->callbacks_enabled))
    dispatch->callback (dispatch->response, dispatch->user_data);
  return G_SOURCE_REMOVE;
}

static void
callback_dispatch_free (gpointer user_data)
{
  MercuryCallbackDispatch *dispatch = user_data;
  g_clear_object (&dispatch->mercury);
  mercury_response_copy_free (dispatch->response);
  g_free (dispatch);
}

static MercuryResponse *
mercury_response_copy (const MercuryResponse *response)
{
  MercuryResponse *copy = g_new0 (MercuryResponse, 1);
  copy->status_code = response->status_code;
  copy->uri = g_strdup (response->uri);
  copy->parts = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  for (guint i = 0; response->parts && i < response->parts->len; i++)
    g_ptr_array_add (copy->parts,
                     g_bytes_ref (g_ptr_array_index (response->parts, i)));
  return copy;
}

static void
dispatch_response (SpotifyMercury *self, GMainContext *context,
                   MercuryCallback callback,
                   gpointer user_data, const MercuryResponse *response)
{
  if (!callback)
    return;

  MercuryCallbackDispatch *dispatch = g_new0 (MercuryCallbackDispatch, 1);
  dispatch->mercury = g_object_ref (self);
  dispatch->callback = callback;
  dispatch->user_data = user_data;
  dispatch->response = mercury_response_copy (response);
  g_main_context_invoke_full (context, G_PRIORITY_DEFAULT,
                              run_callback_dispatch, dispatch,
                              callback_dispatch_free);
}

static void
request_dispatch_free (gpointer user_data)
{
  MercuryRequestDispatch *request = user_data;
  g_clear_object (&request->mercury);
  g_free (request->method_override);
  g_free (request->uri);
  g_clear_pointer (&request->payload, g_bytes_unref);
  g_strfreev (request->field_keys);
  g_strfreev (request->field_values);
  g_clear_pointer (&request->parts, g_ptr_array_unref);
  g_clear_pointer (&request->callback_context, g_main_context_unref);
  g_free (request);
}

static void
subscription_free (gpointer data)
{
  Subscription *sub = data;
  if (!sub) return;
  g_free (sub->uri);
  g_clear_pointer (&sub->callback_context, g_main_context_unref);
  g_free (sub);
}

G_DEFINE_FINAL_TYPE (SpotifyMercury, spotifygtk_mercury, G_TYPE_OBJECT)

/* ── Wire encoding ──────────────────────────────────────────────────────── */

static const gchar *
method_string (MercuryMethod method)
{
  switch (method) {
    case MERCURY_METHOD_SUB:   return "SUB";
    case MERCURY_METHOD_UNSUB: return "UNSUB";
    case MERCURY_METHOD_SEND:  return "SEND";
    default:                   return "GET";
  }
}

static void
append_u16_be (GByteArray *buf, guint16 v)
{
  guint8 be[2] = { (guint8) (v >> 8), (guint8) (v & 0xff) };
  g_byte_array_append (buf, be, 2);
}

static GByteArray *
encode_mercury_packet (guint64 seq, MercuryMethod method, const gchar *method_override,
                       const gchar *uri, const gchar *content_type, GBytes *payload,
                       const gchar *const *field_keys,
                       const gchar *const *field_values, guint n_fields)
{
  GByteArray *buf = g_byte_array_new ();

  guint8 seq_be[8];
  for (int i = 0; i < 8; i++) seq_be[7 - i] = (guint8) (seq >> (8 * i));
  append_u16_be (buf, sizeof (seq_be));
  g_byte_array_append (buf, seq_be, sizeof (seq_be));

  g_byte_array_append (buf, (const guint8[]) { MERCURY_FLAG_FINAL }, 1);
  append_u16_be (buf, (guint16) (payload ? 2 : 1));

  g_autoptr(GByteArray) header = g_byte_array_new ();
  pb_write_bytes_field (header, MERCURY_HDR_URI,
                        (const guint8 *) uri, strlen (uri));
  const gchar *m = method_override ? method_override : method_string (method);
  pb_write_bytes_field (header, MERCURY_HDR_METHOD,
                        (const guint8 *) m, strlen (m));

  /*
   * Header.content_type. Never set until now, and it appears to be how a
   * service selects a schema: the official client carries
   * "application/vnd.collection-v2.spotify.proto" for collection traffic,
   * which is a string that exists in its binary and nowhere in any public
   * schema. Without it the collection endpoint answers in an older shape that
   * matches no published proto.
   */
  if (content_type && *content_type)
    pb_write_bytes_field (header, MERCURY_HDR_CONTENT_TYPE,
                          (const guint8 *) content_type, strlen (content_type));

  /*
   * Header.user_fields. Spotify's own client carries "Collection-Update-Id"
   * here for collection writes -- the string sits directly beside
   * hm://collection/%s/ in its binary. That is why every attempt to express an
   * update through the *body* failed: the marker was never a body field.
   */
  for (guint i = 0; i < n_fields; i++) {
    g_autoptr(GByteArray) uf = g_byte_array_new ();
    pb_write_bytes_field (uf, MERCURY_UF_KEY,
                          (const guint8 *) field_keys[i], strlen (field_keys[i]));
    pb_write_bytes_field (uf, MERCURY_UF_VALUE,
                          (const guint8 *) field_values[i], strlen (field_values[i]));
    pb_write_message_field (header, MERCURY_HDR_USER_FIELDS, uf->data, uf->len);
  }

  append_u16_be (buf, (guint16) header->len);
  g_byte_array_append (buf, header->data, header->len);

  if (payload) {
    gsize psize = 0;
    const guint8 *pdata = g_bytes_get_data (payload, &psize);
    append_u16_be (buf, (guint16) psize);
    g_byte_array_append (buf, pdata, psize);
  }

  return buf;
}

static ApCommandId
cmd_for_method (MercuryMethod method)
{
  switch (method) {
    case MERCURY_METHOD_SUB:   return AP_CMD_MERCURY_SUB;
    case MERCURY_METHOD_UNSUB: return AP_CMD_MERCURY_UNSUB;
    default:                   return AP_CMD_MERCURY_REQ;
  }
}

/*
 * Measured against a live server, not guessed: a single Mercury request of
 * 16000 bytes is answered and 16400 is not, so the ceiling is 16384 -- one
 * KiB-aligned receive buffer. The frame also carries the sequence number,
 * flags, part count, and the Header protobuf, so leave room for those.
 */
#define MERCURY_MAX_REQUEST   16384
#define MERCURY_FRAME_OVERHEAD  128

/*
 * Send a payload as several parts in one packet. A part's length is a u16, so
 * anything over 65535 bytes cannot travel as one; splitting at item
 * boundaries works because concatenated protobuf messages with the same
 * repeated field merge, so the server sees one list however it is divided.
 */
void
spotifygtk_mercury_set_content_type (SpotifyMercury *self, const gchar *content_type)
{
  g_return_if_fail (SPOTIFYGTK_IS_MERCURY (self));
  g_mutex_lock (&self->config_lock);
  g_free (self->content_type);
  self->content_type = g_strdup (content_type);
  g_mutex_unlock (&self->config_lock);
}

static void
send_request_parts_on_owner (MercuryRequestDispatch *request)
{
  SpotifyMercury *self = request->mercury;
  GPtrArray *parts = request->parts;

  guint64 seq = self->next_seq++;

  g_autoptr(GByteArray) buf = g_byte_array_new ();
  guint8 seq_be[8];
  for (int i = 0; i < 8; i++) seq_be[7 - i] = (guint8) (seq >> (8 * i));
  append_u16_be (buf, sizeof (seq_be));
  g_byte_array_append (buf, seq_be, sizeof (seq_be));
  g_byte_array_append (buf, (const guint8[]) { MERCURY_FLAG_FINAL }, 1);
  append_u16_be (buf, (guint16) (1 + parts->len));

  g_autoptr(GByteArray) header = g_byte_array_new ();
  pb_write_bytes_field (header, MERCURY_HDR_URI,
                        (const guint8 *) request->uri, strlen (request->uri));
  const gchar *m = request->method_override
                     ? request->method_override : method_string (request->method);
  pb_write_bytes_field (header, MERCURY_HDR_METHOD, (const guint8 *) m, strlen (m));
  g_mutex_lock (&self->config_lock);
  g_autofree gchar *ct = g_strdup (self->content_type);
  g_mutex_unlock (&self->config_lock);
  if (ct && *ct)
    pb_write_bytes_field (header, MERCURY_HDR_CONTENT_TYPE,
                          (const guint8 *) ct, strlen (ct));

  append_u16_be (buf, (guint16) header->len);
  g_byte_array_append (buf, header->data, header->len);

  for (guint i = 0; i < parts->len; i++) {
    gsize plen = 0;
    const guint8 *pd = g_bytes_get_data (g_ptr_array_index (parts, i), &plen);
    append_u16_be (buf, (guint16) plen);
    g_byte_array_append (buf, pd, plen);
  }

  MercuryPending *p = g_new0 (MercuryPending, 1);
  p->parts     = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  p->callback  = request->callback;
  p->user_data = request->user_data;
  p->callback_context = g_main_context_ref (request->callback_context);
  g_hash_table_insert (self->pending, g_memdup2 (&seq, sizeof (seq)), p);

  g_message ("mercury: sending %u payload part(s), %u bytes total",
             parts->len, buf->len);
  spotifygtk_ap_session_send (self->ap_session, cmd_for_method (request->method),
                              buf->data, buf->len);
}

static void send_request_fields_on_owner (MercuryRequestDispatch *request);

static gboolean
run_request_dispatch (gpointer user_data)
{
  MercuryRequestDispatch *request = user_data;
  if (request->multipart)
    send_request_parts_on_owner (request);
  else
    send_request_fields_on_owner (request);
  return G_SOURCE_REMOVE;
}

static void
queue_request_dispatch (MercuryRequestDispatch *request)
{
  SpotifyMercury *self = request->mercury;
  if (g_main_context_is_owner (self->owner_context)) {
    run_request_dispatch (request);
    request_dispatch_free (request);
    return;
  }

  g_main_context_invoke_full (self->owner_context, G_PRIORITY_DEFAULT,
                              run_request_dispatch, request,
                              request_dispatch_free);
}

void
spotifygtk_mercury_request_parts (SpotifyMercury *self, MercuryMethod method,
                                  const gchar *method_override, const gchar *uri,
                                  GPtrArray *parts, MercuryCallback callback,
                                  gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_MERCURY (self));
  g_return_if_fail (uri != NULL && parts != NULL);

  MercuryRequestDispatch *request = g_new0 (MercuryRequestDispatch, 1);
  request->mercury = g_object_ref (self);
  request->method = method;
  request->method_override = g_strdup (method_override);
  request->uri = g_strdup (uri);
  request->parts = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  for (guint i = 0; i < parts->len; i++)
    g_ptr_array_add (request->parts,
                     g_bytes_ref (g_ptr_array_index (parts, i)));
  request->callback = callback;
  request->user_data = user_data;
  request->callback_context = g_main_context_ref_thread_default ();
  request->multipart = TRUE;
  queue_request_dispatch (request);
}

void
spotifygtk_mercury_request (SpotifyMercury *self, MercuryMethod method, const gchar *uri,
                            GBytes *payload, MercuryCallback callback, gpointer user_data)
{
  spotifygtk_mercury_request_full (self, method, NULL, uri, payload, callback, user_data);
}

/*
 * The Header's `method` is a free-form string, not one of the four enum names.
 * That distinction turns out to matter: a collection write to the right URI is
 * refused with 405 (Method Not Allowed) when the string is "SEND", because the
 * AP command that carries a write and the verb the service expects to read in
 * the header are two different things.
 */
void
spotifygtk_mercury_request_full (SpotifyMercury *self, MercuryMethod method,
                                 const gchar *method_override, const gchar *uri,
                                 GBytes *payload, MercuryCallback callback,
                                 gpointer user_data)
{
  spotifygtk_mercury_request_fields (self, method, method_override, uri, payload,
                                     NULL, NULL, 0, callback, user_data);
}

static void
send_request_fields_on_owner (MercuryRequestDispatch *request)
{
  SpotifyMercury *self = request->mercury;

  guint64 seq = self->next_seq++;

  /*
   * A part's length is a u16, so a payload over 65535 bytes cannot travel as
   * one, and in practice anything past ~16KB in a single packet is dropped
   * without a reply. Mercury's answer is fragmentation: one sequence number
   * spans several packets, each flagged PARTIAL, and the receiver joins the
   * trailing fragment of each packet to the head of the next.
   *
   * The layout is derived from librespot's dispatch loop, which is the only
   * description of the format that is known to work:
   *
   *   packet 1  flags=PARTIAL count=2  [header][chunk 0]
   *   packet n  flags=PARTIAL count=1  [chunk n]
   *   last      flags=FINAL   count=1  [chunk N]
   *
   * At each packet the receiver takes the last part when flags is PARTIAL and
   * holds it as `partial`; the next packet's first part is appended to it. So
   * the header must be a complete part in packet 1 (it is not last there), and
   * only the payload is ever split.
   */
  gsize payload_len = 0;
  if (request->payload) g_bytes_get_data (request->payload, &payload_len);

  if (payload_len + strlen (request->uri) + MERCURY_FRAME_OVERHEAD > MERCURY_MAX_REQUEST) {
    /*
     * Refused rather than sent, because the failure mode otherwise is silence:
     * the server drops an oversized request without any reply, the callback
     * never fires, and the caller waits forever on something that was never
     * going to be answered. Hours went into chasing that as a rate limit.
     *
     * Splitting is not an option. Fragmentation (flags = PARTIAL across
     * packets, which the *receive* path handles for responses) is not
     * reassembled for inbound requests -- proven by sending an identical
     * 12000-byte body two ways to the same endpoint: one packet answered 200,
     * two fragments got nothing. Multiple parts inside one packet do not help
     * either, since the limit is on the whole frame.
     */
    g_warning ("mercury: refusing a %" G_GSIZE_FORMAT "-byte request to %s; the "
               "server accepts at most %d bytes per request and does not "
               "reassemble fragmented ones", payload_len, request->uri,
               MERCURY_MAX_REQUEST);
    if (request->callback) {
      MercuryResponse err = { .status_code = 413, .uri = request->uri,
                              .parts = g_ptr_array_new () };
      dispatch_response (self, request->callback_context, request->callback,
                         request->user_data, &err);
      g_ptr_array_unref (err.parts);
    }
    return;
  }

  g_mutex_lock (&self->config_lock);
  g_autofree gchar *content_type = g_strdup (self->content_type);
  g_mutex_unlock (&self->config_lock);

  g_autoptr(GByteArray) packet =
    encode_mercury_packet (seq, request->method, request->method_override,
                           request->uri, content_type, request->payload,
                           (const gchar *const *) request->field_keys,
                           (const gchar *const *) request->field_values,
                           request->n_fields);

  /*
   * The pending entry is filed even with no callback: a reply still arrives
   * and has to be absorbed rather than logged as unsolicited. It is removed
   * when the message completes.
   */
  MercuryPending *p = g_new0 (MercuryPending, 1);
  p->parts     = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  p->callback  = request->callback;
  p->user_data = request->user_data;
  p->callback_context = g_main_context_ref (request->callback_context);
  g_hash_table_insert (self->pending, g_memdup2 (&seq, sizeof (seq)), p);

  spotifygtk_ap_session_send (self->ap_session, cmd_for_method (request->method),
                              packet->data, packet->len);
}

void
spotifygtk_mercury_request_fields (SpotifyMercury *self, MercuryMethod method,
                                   const gchar *method_override, const gchar *uri,
                                   GBytes *payload,
                                   const gchar *const *field_keys,
                                   const gchar *const *field_values, guint n_fields,
                                   MercuryCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_MERCURY (self));
  g_return_if_fail (uri != NULL);

  MercuryRequestDispatch *request = g_new0 (MercuryRequestDispatch, 1);
  request->mercury = g_object_ref (self);
  request->method = method;
  request->method_override = g_strdup (method_override);
  request->uri = g_strdup (uri);
  request->payload = payload ? g_bytes_ref (payload) : NULL;
  request->n_fields = n_fields;
  if (n_fields > 0) {
    request->field_keys = g_new0 (gchar *, n_fields + 1);
    request->field_values = g_new0 (gchar *, n_fields + 1);
    for (guint i = 0; i < n_fields; i++) {
      request->field_keys[i] = g_strdup (field_keys[i]);
      request->field_values[i] = g_strdup (field_values[i]);
    }
  }
  request->callback = callback;
  request->user_data = user_data;
  request->callback_context = g_main_context_ref_thread_default ();
  queue_request_dispatch (request);
}

static void
on_subscribe_reply (MercuryResponse *response, gpointer user_data)
{
  (void) user_data;
  if (!response) return;
  g_message ("mercury: SUB %s -> status %d (%u part(s))",
             response->uri ? response->uri : "(no uri)",
             response->status_code, response->parts ? response->parts->len : 0);
}

static gboolean
run_subscription_dispatch (gpointer user_data)
{
  MercurySubscriptionDispatch *dispatch = user_data;
  SpotifyMercury *self = dispatch->mercury;

  if (dispatch->subscription) {
    Subscription *sub = g_steal_pointer (&dispatch->subscription);
    g_hash_table_insert (self->subscriptions, g_strdup (sub->uri), sub);
    spotifygtk_mercury_request (self, MERCURY_METHOD_SUB, sub->uri, NULL,
                                on_subscribe_reply, NULL);
    return G_SOURCE_REMOVE;
  }

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->subscriptions);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    Subscription *sub = value;
    if (sub->id == dispatch->unsubscribe_id) {
      spotifygtk_mercury_request (self, MERCURY_METHOD_UNSUB, sub->uri,
                                  NULL, NULL, NULL);
      g_hash_table_iter_remove (&iter);
      break;
    }
  }
  return G_SOURCE_REMOVE;
}

static void
subscription_dispatch_free (gpointer user_data)
{
  MercurySubscriptionDispatch *dispatch = user_data;
  g_clear_object (&dispatch->mercury);
  subscription_free (dispatch->subscription);
  g_free (dispatch);
}

static void
queue_subscription_dispatch (MercurySubscriptionDispatch *dispatch)
{
  SpotifyMercury *self = dispatch->mercury;
  if (g_main_context_is_owner (self->owner_context)) {
    run_subscription_dispatch (dispatch);
    subscription_dispatch_free (dispatch);
    return;
  }

  g_main_context_invoke_full (self->owner_context, G_PRIORITY_DEFAULT,
                              run_subscription_dispatch, dispatch,
                              subscription_dispatch_free);
}

guint64
spotifygtk_mercury_subscribe (SpotifyMercury *self, const gchar *uri,
                              MercuryCallback callback, gpointer user_data)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_MERCURY (self), 0);
  g_return_val_if_fail (uri != NULL, 0);

  Subscription *sub = g_new0 (Subscription, 1);
  g_mutex_lock (&self->config_lock);
  sub->id        = self->next_sub_id++;
  g_mutex_unlock (&self->config_lock);
  sub->uri       = g_strdup (uri);
  sub->callback  = callback;
  sub->user_data = user_data;
  sub->callback_context = g_main_context_ref_thread_default ();

  /*
   * The SUB reply is logged rather than discarded. For a service whose
   * endpoints are undocumented, whether a subscribe is accepted at all is
   * itself the finding: a 200 says the URI names something real, and anything
   * else says stop waiting for events that will never come.
   */
  guint64 id = sub->id;
  MercurySubscriptionDispatch *dispatch =
    g_new0 (MercurySubscriptionDispatch, 1);
  dispatch->mercury = g_object_ref (self);
  dispatch->subscription = sub;
  queue_subscription_dispatch (dispatch);
  return id;
}

void
spotifygtk_mercury_unsubscribe (SpotifyMercury *self, guint64 sub_id)
{
  g_return_if_fail (SPOTIFYGTK_IS_MERCURY (self));

  MercurySubscriptionDispatch *dispatch =
    g_new0 (MercurySubscriptionDispatch, 1);
  dispatch->mercury = g_object_ref (self);
  dispatch->unsubscribe_id = sub_id;
  queue_subscription_dispatch (dispatch);
}

void
mercury_response_free (MercuryResponse *response)
{
  if (!response) return;
  if (response->parts) g_ptr_array_free (response->parts, TRUE);
  g_free (response->uri);
  g_free (response);
}

/* ── Incoming dispatch ──────────────────────────────────────────────────── */

/*
 * Complete a message: part 0 is the Header, the rest is payload. Consumes
 * `p` -- the caller has already detached it from the pending table.
 */
static void
complete_message (SpotifyMercury *self, ApCommandId cmd, MercuryPending *p)
{
  if (p->parts->len == 0) {
    g_warning ("mercury: message completed with no header part; dropping");
    mercury_pending_free (p);
    return;
  }

  GBytes *header_bytes = g_ptr_array_index (p->parts, 0);
  gsize hlen = 0;
  const guint8 *hdata = g_bytes_get_data (header_bytes, &hlen);

  g_autofree gchar *uri = NULL;
  const guint8 *uri_data = NULL;
  gsize uri_len = 0;
  if (pb_find_bytes_field (hdata, hlen, MERCURY_HDR_URI, &uri_data, &uri_len))
    uri = g_strndup ((const gchar *) uri_data, uri_len);

  /* status_code is sint32, so the varint is zigzag-encoded. Reading it as a
   * plain varint turns 200 into 400 and every error code into a positive. */
  gint32 status = 0;
  guint64 raw = 0;
  if (pb_find_varint_field (hdata, hlen, MERCURY_HDR_STATUS_CODE, &raw))
    status = (gint32) ((raw >> 1) ^ (guint64) (-(gint64) (raw & 1)));

  MercuryResponse response = {
    .status_code = status,
    .uri         = uri,
    .parts       = p->parts,
  };
  g_ptr_array_remove_index (response.parts, 0);   /* drop the header part */

  /*
   * Event payloads can be dumped for inspection. A published event is the
   * server describing a change in its own words, which is the closest thing
   * available to a specification for the update format -- worth capturing
   * rather than counting parts and discarding.
   */
  if (cmd == AP_CMD_MERCURY_EVENT) {
    const gchar *dir = g_getenv ("SPOTIFY_DUMP_EVENTS");
    if (dir && *dir && response.parts) {
      for (guint i = 0; i < response.parts->len; i++) {
        gsize len = 0;
        const guint8 *d = g_bytes_get_data (g_ptr_array_index (response.parts, i), &len);
        g_autofree gchar *safe = g_strdup (uri ? uri : "no-uri");
        for (gchar *c = safe; *c; c++) if (*c == '/' || *c == ':') *c = '_';
        g_autofree gchar *fn = g_strdup_printf ("%s/evt-%s-%u.bin", dir, safe, i);
        g_file_set_contents (fn, (const gchar *) d, (gssize) len, NULL);
        g_message ("mercury: event payload %" G_GSIZE_FORMAT " bytes -> %s", len, fn);
      }
    }
  }

  if (p->callback) {
    dispatch_response (self, p->callback_context, p->callback, p->user_data,
                       &response);
  } else if (cmd == AP_CMD_MERCURY_EVENT) {
    /*
     * An unsolicited event. Exact URI first, then prefix: Spotify publishes
     * to a more specific URI than the one subscribed to.
     */
    Subscription *match = g_hash_table_lookup (self->subscriptions, uri ? uri : "");
    if (!match && uri) {
      GHashTableIter iter;
      gpointer k, v;
      g_hash_table_iter_init (&iter, self->subscriptions);
      while (g_hash_table_iter_next (&iter, &k, &v)) {
        if (g_str_has_prefix (uri, (const gchar *) k)) { match = v; break; }
      }
    }

    if (match && match->callback)
      dispatch_response (self, match->callback_context, match->callback,
                         match->user_data, &response);
    else
      /* Worth seeing rather than dropping: the URI of an event nobody asked
       * for is how an undocumented service announces its own name. */
      g_message ("mercury: unsolicited event for %s (%u payload part(s), status %d)",
                 uri ? uri : "(no uri)", response.parts->len, status);
  }

  g_ptr_array_unref (response.parts);
  g_clear_pointer (&p->partial, g_byte_array_unref);
  g_clear_pointer (&p->callback_context, g_main_context_unref);
  g_free (p);
}

static void
on_mercury_packet (SpotifyApSession *session, ApCommandId cmd,
                   const guint8 *payload, gsize len, gpointer user_data)
{
  SpotifyMercury *self = user_data;
  (void) session;

  gsize pos = 0;
  if (len < 2) goto malformed;
  guint16 seq_len = (guint16) ((payload[0] << 8) | payload[1]);
  pos = 2;

  if (len < pos + seq_len + 3) goto malformed;

  /* Every sequence number this client originates is 8 bytes, so anything else
   * cannot be a reply to us. Treated as an event so it is still reported. */
  guint64 seq = 0;
  gboolean seq_usable = (seq_len == 8);
  for (guint16 i = 0; i < seq_len; i++)
    seq = (seq << 8) | payload[pos + i];
  pos += seq_len;

  guint8  flags = payload[pos++];
  guint16 count = (guint16) ((payload[pos] << 8) | payload[pos + 1]);
  pos += 2;

  MercuryPending *p = NULL;
  if (seq_usable)
    p = g_hash_table_lookup (self->pending, &seq);

  if (!p) {
    if (cmd != AP_CMD_MERCURY_EVENT) {
      g_debug ("mercury: reply for unknown seq (cmd 0x%02x); ignoring", cmd);
      return;
    }
    p = g_new0 (MercuryPending, 1);
    p->parts = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  } else {
    /*
     * Detach without running the value destructor: completion frees `p`, and
     * a continuation re-inserts it below. _steal_extended rather than _steal
     * because plain _steal skips the *key* destructor too, leaking the
     * heap-allocated sequence number on every reply.
     */
    gpointer stolen_key = NULL;
    g_hash_table_steal_extended (self->pending, &seq, &stolen_key, NULL);
    g_free (stolen_key);
  }

  for (guint16 i = 0; i < count; i++) {
    if (pos + 2 > len) goto malformed_mid;
    guint16 size = (guint16) ((payload[pos] << 8) | payload[pos + 1]);
    pos += 2;
    if (pos + size > len) goto malformed_mid;

    /* A fragment carried over from the previous packet is the head of this
     * part, not a part of its own. */
    if (p->partial) {
      g_byte_array_append (p->partial, payload + pos, size);
      if (i == count - 1 && flags == MERCURY_FLAG_PARTIAL) {
        pos += size;
        continue;   /* still incomplete */
      }
      g_ptr_array_add (p->parts,
                       g_byte_array_free_to_bytes (g_steal_pointer (&p->partial)));
    } else if (i == count - 1 && flags == MERCURY_FLAG_PARTIAL) {
      p->partial = g_byte_array_new ();
      g_byte_array_append (p->partial, payload + pos, size);
    } else {
      g_ptr_array_add (p->parts, g_bytes_new (payload + pos, size));
    }
    pos += size;
  }

  if (flags == MERCURY_FLAG_FINAL) {
    complete_message (self, cmd, p);
  } else if (seq_usable) {
    g_hash_table_insert (self->pending, g_memdup2 (&seq, sizeof (seq)), p);
  } else {
    mercury_pending_free (p);   /* no key to file a continuation under */
  }
  return;

malformed_mid:
  g_warning ("mercury: truncated part in a %" G_GSIZE_FORMAT "-byte packet", len);
  mercury_pending_free (p);
  return;

malformed:
  g_warning ("mercury: packet too short (%" G_GSIZE_FORMAT " bytes)", len);
}

static void
spotifygtk_mercury_dispose (GObject *object)
{
  SpotifyMercury *self = SPOTIFYGTK_MERCURY (object);

  /*
   * Take the packet handlers down before anything they read goes away.
   *
   * new() registers four of them with `self` as their user data, and the AP
   * session outlives this object -- it is shared, and this only ever held a
   * reference. So a Mercury packet arriving after the last unref called
   * on_mercury_packet with a freed `self`, whose first act is to look up
   * self->pending: a segfault inside g_hash_table_lookup, from a socket
   * callback, with nothing of ours on the stack above it.
   *
   * Clearing a handler is set_handler with NULL, which the dispatcher checks
   * for; the same thing the login path does when it is finished with its own.
   */
  if (self->ap_session) {
    spotifygtk_ap_session_set_handler (self->ap_session, AP_CMD_MERCURY_REQ,   NULL, NULL);
    spotifygtk_ap_session_set_handler (self->ap_session, AP_CMD_MERCURY_SUB,   NULL, NULL);
    spotifygtk_ap_session_set_handler (self->ap_session, AP_CMD_MERCURY_UNSUB, NULL, NULL);
    spotifygtk_ap_session_set_handler (self->ap_session, AP_CMD_MERCURY_EVENT, NULL, NULL);
  }

  g_clear_object (&self->ap_session);
  g_clear_pointer (&self->pending,        g_hash_table_unref);
  g_clear_pointer (&self->subscriptions,  g_hash_table_unref);
  g_clear_pointer (&self->owner_context,  g_main_context_unref);
  g_clear_pointer (&self->content_type,   g_free);
  G_OBJECT_CLASS (spotifygtk_mercury_parent_class)->dispose (object);
}

static void
spotifygtk_mercury_finalize (GObject *object)
{
  SpotifyMercury *self = SPOTIFYGTK_MERCURY (object);
  g_mutex_clear (&self->config_lock);
  G_OBJECT_CLASS (spotifygtk_mercury_parent_class)->finalize (object);
}

static void
spotifygtk_mercury_class_init (SpotifyMercuryClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_mercury_dispose;
  G_OBJECT_CLASS (klass)->finalize = spotifygtk_mercury_finalize;
}

static void
spotifygtk_mercury_init (SpotifyMercury *self)
{
  g_mutex_init (&self->config_lock);
  self->pending       = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                               g_free, mercury_pending_free);
  self->subscriptions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, subscription_free);
  self->next_seq      = 0;
  self->next_sub_id   = 1;
  self->callbacks_enabled = TRUE;
}

void
spotifygtk_mercury_cancel_callbacks (SpotifyMercury *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_MERCURY (self));
  g_atomic_int_set (&self->callbacks_enabled, FALSE);
}

SpotifyMercury *
spotifygtk_mercury_new (SpotifyApSession *ap_session)
{
  SpotifyMercury *self = g_object_new (SPOTIFYGTK_TYPE_MERCURY, NULL);
  self->ap_session = g_object_ref (ap_session);
  self->owner_context = g_main_context_ref_thread_default ();

  /*
   * Registered here rather than left to the caller. The previous arrangement
   * documented this as the owner's job and no owner ever did it, so every
   * reply and every event was discarded and no callback ever fired.
   *
   * Replies to GET/SEND come back as MERCURY_REQ; SUB and UNSUB are answered
   * the same way, and published events arrive as MERCURY_EVENT.
   */
  spotifygtk_ap_session_set_handler (ap_session, AP_CMD_MERCURY_REQ,   on_mercury_packet, self);
  spotifygtk_ap_session_set_handler (ap_session, AP_CMD_MERCURY_SUB,   on_mercury_packet, self);
  spotifygtk_ap_session_set_handler (ap_session, AP_CMD_MERCURY_UNSUB, on_mercury_packet, self);
  spotifygtk_ap_session_set_handler (ap_session, AP_CMD_MERCURY_EVENT, on_mercury_packet, self);

  return self;
}

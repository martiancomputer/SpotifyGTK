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

/* flags byte */
#define MERCURY_FLAG_FINAL   1
#define MERCURY_FLAG_PARTIAL 2

struct _SpotifyMercury {
  GObject           parent_instance;
  SpotifyApSession *ap_session;

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
} MercuryPending;

static void
mercury_pending_free (gpointer data)
{
  MercuryPending *p = data;
  if (!p) return;
  g_clear_pointer (&p->parts,   g_ptr_array_unref);
  g_clear_pointer (&p->partial, g_byte_array_unref);
  g_free (p);
}

static void
subscription_free (gpointer data)
{
  Subscription *sub = data;
  if (!sub) return;
  g_free (sub->uri);
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
encode_mercury_packet (guint64 seq, MercuryMethod method, const gchar *uri, GBytes *payload)
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
  const gchar *m = method_string (method);
  pb_write_bytes_field (header, MERCURY_HDR_METHOD,
                        (const guint8 *) m, strlen (m));

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

void
spotifygtk_mercury_request (SpotifyMercury *self, MercuryMethod method, const gchar *uri,
                            GBytes *payload, MercuryCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_MERCURY (self));
  g_return_if_fail (uri != NULL);

  guint64 seq = self->next_seq++;
  g_autoptr(GByteArray) packet = encode_mercury_packet (seq, method, uri, payload);

  /*
   * The pending entry is filed even with no callback: a reply still arrives
   * and has to be absorbed rather than logged as unsolicited. It is removed
   * when the message completes.
   */
  MercuryPending *p = g_new0 (MercuryPending, 1);
  p->parts     = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  p->callback  = callback;
  p->user_data = user_data;
  g_hash_table_insert (self->pending, g_memdup2 (&seq, sizeof (seq)), p);

  spotifygtk_ap_session_send (self->ap_session, cmd_for_method (method),
                              packet->data, packet->len);
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

guint64
spotifygtk_mercury_subscribe (SpotifyMercury *self, const gchar *uri,
                              MercuryCallback callback, gpointer user_data)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_MERCURY (self), 0);

  Subscription *sub = g_new0 (Subscription, 1);
  sub->id        = self->next_sub_id++;
  sub->uri       = g_strdup (uri);
  sub->callback  = callback;
  sub->user_data = user_data;

  g_hash_table_insert (self->subscriptions, g_strdup (uri), sub);

  /*
   * The SUB reply is logged rather than discarded. For a service whose
   * endpoints are undocumented, whether a subscribe is accepted at all is
   * itself the finding: a 200 says the URI names something real, and anything
   * else says stop waiting for events that will never come.
   */
  spotifygtk_mercury_request (self, MERCURY_METHOD_SUB, uri, NULL,
                              on_subscribe_reply, self);
  return sub->id;
}

void
spotifygtk_mercury_unsubscribe (SpotifyMercury *self, guint64 sub_id)
{
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->subscriptions);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    Subscription *sub = value;
    if (sub->id == sub_id) {
      spotifygtk_mercury_request (self, MERCURY_METHOD_UNSUB, sub->uri, NULL, NULL, NULL);
      g_hash_table_iter_remove (&iter);
      return;
    }
  }
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

  if (p->callback) {
    p->callback (&response, p->user_data);
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

    if (match)
      match->callback (&response, match->user_data);
    else
      /* Worth seeing rather than dropping: the URI of an event nobody asked
       * for is how an undocumented service announces its own name. */
      g_message ("mercury: unsolicited event for %s (%u payload part(s), status %d)",
                 uri ? uri : "(no uri)", response.parts->len, status);
  }

  g_ptr_array_unref (response.parts);
  g_clear_pointer (&p->partial, g_byte_array_unref);
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
  g_clear_object (&self->ap_session);
  g_clear_pointer (&self->pending,        g_hash_table_unref);
  g_clear_pointer (&self->subscriptions,  g_hash_table_unref);
  G_OBJECT_CLASS (spotifygtk_mercury_parent_class)->dispose (object);
}

static void
spotifygtk_mercury_class_init (SpotifyMercuryClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_mercury_dispose;
}

static void
spotifygtk_mercury_init (SpotifyMercury *self)
{
  self->pending       = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                               g_free, mercury_pending_free);
  self->subscriptions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, subscription_free);
  self->next_seq      = 0;
  self->next_sub_id   = 1;
}

SpotifyMercury *
spotifygtk_mercury_new (SpotifyApSession *ap_session)
{
  SpotifyMercury *self = g_object_new (SPOTIFYGTK_TYPE_MERCURY, NULL);
  self->ap_session = g_object_ref (ap_session);

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

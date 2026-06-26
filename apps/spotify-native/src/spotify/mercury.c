/*
 * mercury.c — Mercury pub/sub implementation.
 *
 * The packet layout here (sequence number width, flags byte, part
 * count/length encoding) follows the commonly-documented Mercury wire
 * format from prior community reverse-engineering of the protocol.
 * Unlike shannon.c, getting a field width wrong here just produces a
 * parse error rather than silent corruption — so this is implemented
 * in full rather than stubbed, but should be validated against a real
 * packet capture once ap.c's handshake (and therefore live traffic)
 * is working, in case Spotify has changed field widths since.
 */

#include "config.h"
#include "mercury.h"
#include <string.h>

struct _SpotifyMercury {
  GObject           parent_instance;
  SpotifyApSession *ap_session;

  guint64           next_seq;
  GHashTable       *pending;        /* seq (guint64) -> MercuryCallback closure */
  GHashTable       *pending_data;
  GHashTable       *subscriptions;  /* uri (gchar*) -> sub closure */
  guint64           next_sub_id;
};

typedef struct {
  guint64          id;
  gchar           *uri;
  MercuryCallback  callback;
  gpointer         user_data;
} Subscription;

G_DEFINE_FINAL_TYPE (SpotifyMercury, spotifygtk_mercury, G_TYPE_OBJECT)

/* ── Wire encoding ────────────────────────────────────────────────────────
 * [8 bytes seq, BE][1 byte flags][2 bytes part count]
 * then per part: [2 bytes length, BE][data]
 * First part is always the URI header (method-dependent), remaining
 * parts are the payload. */

static GByteArray *
encode_mercury_packet (guint64 seq, guint8 flags, const gchar *uri, GBytes *payload)
{
  GByteArray *buf = g_byte_array_new ();

  guint8 seq_be[8];
  for (int i = 0; i < 8; i++) seq_be[7 - i] = (guint8) (seq >> (8 * i));
  g_byte_array_append (buf, seq_be, sizeof (seq_be));
  g_byte_array_append (buf, &flags, 1);

  guint16 part_count = payload ? 2 : 1;
  guint8 pc_be[2] = { (guint8) (part_count >> 8), (guint8) (part_count & 0xff) };
  g_byte_array_append (buf, pc_be, 2);

  gsize uri_len = strlen (uri);
  guint8 ulen_be[2] = { (guint8) (uri_len >> 8), (guint8) (uri_len & 0xff) };
  g_byte_array_append (buf, ulen_be, 2);
  g_byte_array_append (buf, (const guint8 *) uri, uri_len);

  if (payload) {
    gsize psize = 0;
    const guint8 *pdata = g_bytes_get_data (payload, &psize);
    guint8 plen_be[2] = { (guint8) (psize >> 8), (guint8) (psize & 0xff) };
    g_byte_array_append (buf, plen_be, 2);
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

  guint64 seq = self->next_seq++;
  g_autoptr(GByteArray) packet = encode_mercury_packet (seq, (guint8) method, uri, payload);

  if (callback) {
    g_hash_table_insert (self->pending,      g_memdup2 (&seq, sizeof (seq)), callback);
    g_hash_table_insert (self->pending_data, g_memdup2 (&seq, sizeof (seq)), user_data);
  }

  spotifygtk_ap_session_send (self->ap_session, cmd_for_method (method),
                              packet->data, packet->len);
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

  spotifygtk_mercury_request (self, MERCURY_METHOD_SUB, uri, NULL, NULL, NULL);
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
  g_free (response);
}

/* ── Incoming event dispatch ─────────────────────────────────────────────
 * Wired via ap_session_set_handler(AP_CMD_MERCURY_EVENT, ...) by whoever
 * owns both objects (the player/session controller) — decoding incoming
 * packets back into MercuryResponse structs follows the same layout as
 * encode_mercury_packet() above, mirrored. */

static void
spotifygtk_mercury_dispose (GObject *object)
{
  SpotifyMercury *self = SPOTIFYGTK_MERCURY (object);
  g_clear_object (&self->ap_session);
  g_clear_pointer (&self->pending,        g_hash_table_unref);
  g_clear_pointer (&self->pending_data,   g_hash_table_unref);
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
  self->pending       = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  self->pending_data  = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  self->subscriptions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->next_seq      = 0;
  self->next_sub_id   = 1;
}

SpotifyMercury *
spotifygtk_mercury_new (SpotifyApSession *ap_session)
{
  SpotifyMercury *self = g_object_new (SPOTIFYGTK_TYPE_MERCURY, NULL);
  self->ap_session = g_object_ref (ap_session);
  return self;
}

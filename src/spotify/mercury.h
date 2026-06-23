/*
 * mercury.h — Mercury pub/sub protocol.
 *
 * Mercury is Spotify's internal request/response + subscription
 * protocol, layered on top of the AP connection's framed packets.
 * Used for metadata lookups (track/album/artist info not covered by
 * the public Web API), playback event subscriptions for Connect, and
 * a few internal RPCs. Each Mercury request is itself a small
 * protobuf-ish header (sequence number, flags, part count) followed
 * by 0+ binary parts.
 */

#pragma once

#include <glib-object.h>
#include "ap.h"

G_BEGIN_DECLS

typedef enum {
  MERCURY_METHOD_GET  = 0,
  MERCURY_METHOD_SUB  = 1,
  MERCURY_METHOD_UNSUB = 2,
  MERCURY_METHOD_SEND = 3,
} MercuryMethod;

typedef struct {
  guint16  status_code;
  GPtrArray *parts;   /* array of GBytes* */
} MercuryResponse;

typedef void (*MercuryCallback) (MercuryResponse *response, gpointer user_data);

#define SPOTIFYGTK_TYPE_MERCURY (spotifygtk_mercury_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyMercury, spotifygtk_mercury, SPOTIFYGTK, MERCURY, GObject)

SpotifyMercury *spotifygtk_mercury_new (SpotifyApSession *ap_session);

/* One-shot request (GET/SEND). */
void spotifygtk_mercury_request (SpotifyMercury *self, MercuryMethod method,
                                 const gchar *uri, GBytes *payload,
                                 MercuryCallback callback, gpointer user_data);

/* Long-lived subscription — callback fires on every published event
 * until spotifygtk_mercury_unsubscribe() is called. */
guint64 spotifygtk_mercury_subscribe (SpotifyMercury *self, const gchar *uri,
                                      MercuryCallback callback, gpointer user_data);
void    spotifygtk_mercury_unsubscribe (SpotifyMercury *self, guint64 sub_id);

void mercury_response_free (MercuryResponse *response);

G_END_DECLS

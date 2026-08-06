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

/*
 * `status_code` is signed because the wire field is sint32: Spotify returns
 * negative codes for some failures, and reading it as unsigned turns those
 * into large positives.
 *
 * `uri` is the one from the response Header, which for a subscription event is
 * the URI actually published to -- often more specific than the one
 * subscribed. NULL if the header carried none. Both it and `parts` are owned
 * by the caller of the callback and valid only for its duration.
 */
typedef struct {
  gint32     status_code;
  gchar     *uri;
  GPtrArray *parts;   /* array of GBytes*, payload only -- the header is removed */
} MercuryResponse;

typedef void (*MercuryCallback) (MercuryResponse *response, gpointer user_data);

#define SPOTIFYGTK_TYPE_MERCURY (spotifygtk_mercury_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyMercury, spotifygtk_mercury, SPOTIFYGTK, MERCURY, GObject)

SpotifyMercury *spotifygtk_mercury_new (SpotifyApSession *ap_session);

/* One-shot request (GET/SEND). */
void spotifygtk_mercury_request (SpotifyMercury *self, MercuryMethod method,
                                 const gchar *uri, GBytes *payload,
                                 MercuryCallback callback, gpointer user_data);

/* As above, but `method_override` replaces the string written into the
 * Header's method field. NULL uses the name of `method`. */
void spotifygtk_mercury_request_full (SpotifyMercury *self, MercuryMethod method,
                                      const gchar *method_override, const gchar *uri,
                                      GBytes *payload, MercuryCallback callback,
                                      gpointer user_data);

/* Send a payload split across several parts in one packet, for bodies over
 * the 65535-byte per-part limit. Split at message boundaries. */
void spotifygtk_mercury_request_parts (SpotifyMercury *self, MercuryMethod method,
                                       const gchar *method_override, const gchar *uri,
                                       GPtrArray *parts, MercuryCallback callback,
                                       gpointer user_data);

/* As _full, plus Header.user_fields entries. Spotify's client uses these for
 * collection writes ("Collection-Update-Id"); they are header metadata, not
 * body fields. */
void spotifygtk_mercury_request_fields (SpotifyMercury *self, MercuryMethod method,
                                        const gchar *method_override, const gchar *uri,
                                        GBytes *payload,
                                        const gchar *const *field_keys,
                                        const gchar *const *field_values, guint n_fields,
                                        MercuryCallback callback, gpointer user_data);

/*
 * Content type for subsequent requests, written into Header.content_type.
 * NULL clears it. A service uses this to pick which schema it answers in --
 * the collection endpoint speaks one shape with
 * "application/vnd.collection-v2.spotify.proto" and an older one without.
 */
void spotifygtk_mercury_set_content_type (SpotifyMercury *self, const gchar *content_type);

/* Long-lived subscription — callback fires on every published event
 * until spotifygtk_mercury_unsubscribe() is called. */
guint64 spotifygtk_mercury_subscribe (SpotifyMercury *self, const gchar *uri,
                                      MercuryCallback callback, gpointer user_data);
void    spotifygtk_mercury_unsubscribe (SpotifyMercury *self, guint64 sub_id);

void mercury_response_free (MercuryResponse *response);

G_END_DECLS

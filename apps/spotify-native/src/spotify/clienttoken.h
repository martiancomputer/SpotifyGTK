/*
 * clienttoken.h — client-token exchange for spclient HTTPS requests.
 *
 * Confirmed against librespot's actual spclient.rs behavior: this is
 * tolerated as optional there ("these endpoints seem to work fine
 * without it") -- a failure here should not block the rest of the
 * chain, only omit the client-token header on subsequent spclient
 * requests. Only the basic ClientDataRequest path is implemented;
 * a ChallengesResponse (HMAC/JS-eval/HashCash proof-of-work) is
 * treated as a soft failure rather than solved, matching that same
 * tolerant behavior.
 *
 * Schema per protocol/proto/spotify/clienttoken/v0/clienttoken_http.proto
 * (proto3 -- plain small-integer field numbers, not the 0xNN-style
 * numbers proto2 messages elsewhere in this codebase use).
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define CLIENTTOKEN_URL "https://clienttoken.spotify.com/v1/clienttoken"

typedef void (*ClientTokenCallback) (const gchar *token /* nullable on failure */,
                                     gpointer user_data);

#define SPOTIFYGTK_TYPE_CLIENT_TOKEN (spotifygtk_client_token_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyClientToken, spotifygtk_client_token,
                      SPOTIFYGTK, CLIENT_TOKEN, GObject)

SpotifyClientToken *spotifygtk_client_token_new (void);

/* client_id here is NATIVE_AUTH_CLIENT_ID (the keymaster client_id),
 * not a dashboard-registered one -- this token request identifies
 * which client is asking, same as everywhere else in this chain. */
void spotifygtk_client_token_request (SpotifyClientToken *self,
                                      const gchar *client_id,
                                      ClientTokenCallback callback,
                                      gpointer user_data);

G_END_DECLS

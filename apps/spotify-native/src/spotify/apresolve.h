/*
 * apresolve.h — Access point discovery.
 *
 * Spotify publishes a list of access points at apresolve.spotify.com. This
 * client previously relied on a DNS SRV lookup instead, which returns a
 * single hostname (ap.single-gslb.spotify.com), so every connection it ever
 * made went to the same place. Because the playback engine opens a fresh AP
 * connection per track, that was reachable within a handful of tracks as
 * "Connection refused", which presents as playback simply stopping working.
 *
 * apresolve returns several distinct hosts (6 endpoints across 4 hostnames
 * when this was written), which gives the rotation in ap.c something real to
 * spread across.
 *
 * Upstream reference: librespot core/src/apresolve.rs.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define APRESOLVE_URL "https://apresolve.spotify.com/?type=accesspoint"

/*
 * Parse an apresolve response body into a NULL-terminated array of
 * "host:port" strings. Free with g_strfreev().
 *
 * Split out from the fetch so it can be tested against captured bodies
 * without touching the network — see tests/test_apresolve.c.
 *
 * Returns NULL and sets `error` if the body is not JSON, is not an object,
 * or carries no usable accesspoint entries.
 */
gchar **spotifygtk_apresolve_parse (const gchar *body, gssize len, GError **error);

/*
 * Fetch the access point list. The result is cached for the lifetime of the
 * process: the list changes rarely, and re-fetching it per connection would
 * add a round trip to something already too eager to open connections.
 *
 * On failure the callback still runs; _finish() returns NULL and the caller
 * is expected to fall back to SRV.
 */
void     spotifygtk_apresolve_get_async  (GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data);
gchar  **spotifygtk_apresolve_get_finish (GAsyncResult *result, GError **error);

G_END_DECLS

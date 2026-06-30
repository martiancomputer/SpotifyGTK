/*
 * print-token.c — extracts the currently stored OAuth access token
 * and prints it to stdout.
 *
 * This exists specifically to answer "how do I get a real token to
 * test spotify-native with" -- the harness's own comment said
 * "your already-working spotify-connect token" without ever
 * explaining how to actually extract one, which led directly to
 * testing spotify-native's login with a Client ID instead of an
 * access token (very different things: the Client ID identifies
 * *which app* is talking to Spotify's API; the access token proves
 * *who's logged in*).
 *
 * Reuses spotify-connect's real spotifygtk_auth_has_valid_token() /
 * spotifygtk_auth_get_token() rather than reimplementing the
 * libsecret/file lookup separately -- if that logic ever changes,
 * this tool doesn't drift out of sync with it.
 *
 * Usage:
 *   ./print-token
 *   export SPOTIFY_ACCESS_TOKEN="$(./print-token)"
 *
 * Exits non-zero with a message on stderr if there's no valid stored
 * token (run spotify-connect itself first to authenticate).
 */

#include "config.h"
#include "auth.h"

#include <glib.h>
#include <stdio.h>

int
main (int argc, char *argv[])
{
  (void) argc; (void) argv;

  SpotifyAuth *auth = spotifygtk_auth_new ();

  if (!spotifygtk_auth_has_valid_token (auth)) {
    g_printerr ("No valid stored token found (or it's expired).\n");
    g_printerr ("Run spotify-connect first and complete the browser login,\n");
    g_printerr ("then try this again.\n");
    g_object_unref (auth);
    return 1;
  }

  const gchar *token = spotifygtk_auth_get_token (auth);
  printf ("%s\n", token);

  g_object_unref (auth);
  return 0;
}

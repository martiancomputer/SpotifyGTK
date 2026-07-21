/*
 * test_apresolve.c — Parsing of the apresolve access-point list.
 *
 * The body below is a real captured response. Parsing is split out from the
 * fetch precisely so this can run offline: the access-point list is the thing
 * standing between the client and "Connection refused", and a parser that
 * silently yields nothing would send every connection back to the single SRV
 * host without any obvious symptom.
 */

#include <glib.h>
#include <string.h>

#include "spotify/apresolve.h"

static void
test_real_response (void)
{
  const gchar *body =
    "{\"accesspoint\":[\"ap-gae2.spotify.com:4070\",\"ap-gae2.spotify.com:443\","
    "\"ap-gae2.spotify.com:80\",\"ap-gue1.spotify.com:4070\","
    "\"ap-gew1.spotify.com:443\",\"ap-gew4.spotify.com:80\"]}";

  g_autoptr(GError) error = NULL;
  g_auto(GStrv) hosts = spotifygtk_apresolve_parse (body, -1, &error);

  g_assert_no_error (error);
  g_assert_nonnull (hosts);
  g_assert_cmpuint (g_strv_length (hosts), ==, 6);
  g_assert_cmpstr (hosts[0], ==, "ap-gae2.spotify.com:4070");
  g_assert_cmpstr (hosts[5], ==, "ap-gew4.spotify.com:80");

  /* The point of the whole exercise: more than one distinct host to spread
   * connections across. A list that is really one host repeated would parse
   * fine and still leave the original problem in place. */
  guint distinct = 0;
  for (guint i = 0; hosts[i]; i++) {
    gboolean seen = FALSE;
    g_auto(GStrv) parts = g_strsplit (hosts[i], ":", 2);
    for (guint j = 0; j < i; j++) {
      if (g_str_has_prefix (hosts[j], parts[0]))
        seen = TRUE;
    }
    if (!seen)
      distinct++;
  }
  g_assert_cmpuint (distinct, >=, 2);
}

/* Entries must carry a port; the connect path splits on ':' and would
 * otherwise dial a hostname with a garbage port. */
static void
test_entries_without_a_port_are_dropped (void)
{
  const gchar *body =
    "{\"accesspoint\":[\"ap-gae2.spotify.com\",\"ap-gue1.spotify.com:4070\"]}";

  g_autoptr(GError) error = NULL;
  g_auto(GStrv) hosts = spotifygtk_apresolve_parse (body, -1, &error);

  g_assert_no_error (error);
  g_assert_cmpuint (g_strv_length (hosts), ==, 1);
  g_assert_cmpstr (hosts[0], ==, "ap-gue1.spotify.com:4070");
}

/* Every failure has to be reported, not returned as an empty list: the
 * caller falls back to SRV on error, and an empty success would instead give
 * the rotation nothing and no indication why. */
static void
test_failures_are_reported (void)
{
  const struct { const gchar *what; const gchar *body; } cases[] = {
    { "not JSON",            "this is not json"                  },
    { "not an object",       "[\"ap.spotify.com:4070\"]"         },
    { "no accesspoint key",  "{\"dealer\":[\"d.spotify.com:443\"]}" },
    { "accesspoint not array", "{\"accesspoint\":\"ap:4070\"}"    },
    { "empty list",          "{\"accesspoint\":[]}"              },
    { "all entries unusable","{\"accesspoint\":[\"noport\",\"\"]}" },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) hosts = spotifygtk_apresolve_parse (cases[i].body, -1, &error);

    if (hosts != NULL)
      g_error ("%s: expected a parse failure, got %u host(s)",
               cases[i].what, g_strv_length (hosts));
    g_assert_nonnull (error);
  }
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/apresolve/real-response", test_real_response);
  g_test_add_func ("/apresolve/no-port-dropped", test_entries_without_a_port_are_dropped);
  g_test_add_func ("/apresolve/failures-reported", test_failures_are_reported);

  return g_test_run ();
}

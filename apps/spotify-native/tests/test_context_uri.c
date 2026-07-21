/*
 * test_context_uri.c — Context URI construction for /context-resolve/v1.
 *
 * These builders take arbitrary user input (a search box) and turn it into
 * both a URI and a request path, so the escaping is the interesting part,
 * not the string formatting. A query containing ':' or '/' must not be
 * able to change how the URI parses or reach a different endpoint.
 *
 * The '+'-for-space rule is transcribed from librespot's spclient.rs
 * doc comment on get_context ("whitespaces are replaced with +").
 */

#include <glib.h>
#include <string.h>

#include "spotify/spclient.h"

static void
test_search_basic (void)
{
  g_autofree gchar *uri = spotifygtk_spclient_build_search_uri ("never gonna");
  g_assert_cmpstr (uri, ==, "spotify:search:never+gonna");
}

static void
test_search_single_word (void)
{
  g_autofree gchar *uri = spotifygtk_spclient_build_search_uri ("radiohead");
  g_assert_cmpstr (uri, ==, "spotify:search:radiohead");
}

/* Tabs and newlines are whitespace too — a pasted query can contain them,
 * and a raw newline in a URI would be malformed. */
static void
test_search_other_whitespace (void)
{
  g_autofree gchar *uri = spotifygtk_spclient_build_search_uri ("a\tb\nc");
  g_assert_cmpstr (uri, ==, "spotify:search:a+b+c");
}

/* The important one: a query must not be able to inject URI structure.
 * ':' has to come back escaped, or "x:collection" would change what the
 * server thinks is being addressed. */
static void
test_search_does_not_allow_uri_injection (void)
{
  g_autofree gchar *uri = spotifygtk_spclient_build_search_uri ("evil:collection");
  g_assert_nonnull (uri);
  g_assert_true (g_str_has_prefix (uri, "spotify:search:"));

  const gchar *query_part = uri + strlen ("spotify:search:");
  g_assert_null (strchr (query_part, ':'));

  g_autofree gchar *slashed = spotifygtk_spclient_build_search_uri ("a/../b");
  g_assert_nonnull (slashed);
  const gchar *slash_part = slashed + strlen ("spotify:search:");
  g_assert_null (strchr (slash_part, '/'));
}

/* '+' is the separator, so it must survive escaping rather than becoming
 * %2B — otherwise multi-word queries break. */
static void
test_search_separator_is_preserved (void)
{
  g_autofree gchar *uri = spotifygtk_spclient_build_search_uri ("two words here");
  g_assert_cmpstr (uri, ==, "spotify:search:two+words+here");
  g_assert_null (strstr (uri, "%2B"));
}

static void
test_search_unicode (void)
{
  g_autofree gchar *uri = spotifygtk_spclient_build_search_uri ("Björk");
  g_assert_nonnull (uri);
  g_assert_true (g_str_has_prefix (uri, "spotify:search:"));
  /* Percent-encoded UTF-8, not raw high bytes. */
  g_assert_nonnull (strstr (uri, "%"));
}

static void
test_search_rejects_empty (void)
{
  g_assert_null (spotifygtk_spclient_build_search_uri (NULL));
  g_assert_null (spotifygtk_spclient_build_search_uri (""));
}

static void
test_collection_uri (void)
{
  g_autofree gchar *uri = spotifygtk_spclient_build_collection_uri ("someuser");
  g_assert_cmpstr (uri, ==, "spotify:user:someuser:collection");

  g_assert_null (spotifygtk_spclient_build_collection_uri (NULL));
  g_assert_null (spotifygtk_spclient_build_collection_uri (""));
}

/* Spotify usernames can be arbitrary strings, so the same injection
 * concern applies: the ":collection" suffix must stay the last segment. */
static void
test_collection_escapes_user_id (void)
{
  g_autofree gchar *uri = spotifygtk_spclient_build_collection_uri ("a:b");
  g_assert_cmpstr (uri, ==, "spotify:user:a%3Ab:collection");
  g_assert_true (g_str_has_suffix (uri, ":collection"));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/context-uri/search-basic", test_search_basic);
  g_test_add_func ("/context-uri/search-single-word", test_search_single_word);
  g_test_add_func ("/context-uri/search-other-whitespace", test_search_other_whitespace);
  g_test_add_func ("/context-uri/search-no-injection", test_search_does_not_allow_uri_injection);
  g_test_add_func ("/context-uri/search-separator-preserved", test_search_separator_is_preserved);
  g_test_add_func ("/context-uri/search-unicode", test_search_unicode);
  g_test_add_func ("/context-uri/search-rejects-empty", test_search_rejects_empty);
  g_test_add_func ("/context-uri/collection", test_collection_uri);
  g_test_add_func ("/context-uri/collection-escapes-user-id", test_collection_escapes_user_id);

  return g_test_run ();
}

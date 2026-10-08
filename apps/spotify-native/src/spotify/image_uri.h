#pragma once
#include <gio/gio.h>
#include <string.h>

/* Catalogue artwork is not an arbitrary URL fetch facility. */
static inline gboolean
spotifygtk_image_uri_allowed (const gchar *url)
{
  if (!url || strlen (url) > 2048) return FALSE;
  g_autoptr(GUri) uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  if (!uri || g_strcmp0 (g_uri_get_scheme (uri), "https") != 0 ||
      g_uri_get_userinfo (uri) || g_uri_get_fragment (uri) ||
      (g_uri_get_port (uri) != -1 && g_uri_get_port (uri) != 443)) return FALSE;
  const gchar *hosts[] = { "i.scdn.co", "mosaic.scdn.co", "misc.scdn.co",
    "image-cdn-ak.spotifycdn.com", "image-cdn-fa.spotifycdn.com",
    "pickasso.spotifycdn.com", "daily-mix.scdn.co" };
  for (guint i = 0; i < G_N_ELEMENTS (hosts); i++)
    if (g_ascii_strcasecmp (g_uri_get_host (uri) ?: "", hosts[i]) == 0)
      return TRUE;
  return FALSE;
}

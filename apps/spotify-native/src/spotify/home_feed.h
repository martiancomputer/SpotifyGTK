#pragma once
#include <json-glib/json-glib.h>
#include <gio/gio.h>

#define SPOTIFYGTK_HOME_MAX_SECTIONS 32
#define SPOTIFYGTK_HOME_MAX_CARDS 10
#define SPOTIFYGTK_HOME_MAX_BYTES (4 * 1024 * 1024)

typedef struct { gchar *uri, *title, *subtitle, *cover; } SpotifyHomeCard;
typedef struct { gchar *title, *subtitle; GPtrArray *cards; } SpotifyHomeSection;
typedef struct { gchar *greeting; GPtrArray *sections; } SpotifyHomeFeed;

void spotifygtk_home_feed_free (SpotifyHomeFeed *feed);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SpotifyHomeFeed, spotifygtk_home_feed_free)
gboolean spotifygtk_home_uri_supported (const gchar *uri);
SpotifyHomeFeed *spotifygtk_home_feed_parse (JsonNode *root, GError **error);
/* Compact snapshots contain display fields only, never tracking payloads. */
GBytes *spotifygtk_home_feed_encode (const SpotifyHomeFeed *feed);
SpotifyHomeFeed *spotifygtk_home_feed_decode (GBytes *bytes);

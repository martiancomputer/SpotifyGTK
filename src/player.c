#include "config.h"
#include "player.h"

struct _SpotifyPlayer {
  GObject      parent_instance;
  SpotifyApi  *api;

  guint        poll_source_id;
  guint        poll_interval_ms;

  gboolean     is_playing;
  gint64       position_ms;
  gint64       duration_ms;
  gint         volume_pct;
  gchar       *track_name;
  gchar       *artist_name;
  gchar       *album_art_url;
};

enum { SIG_STATE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (SpotifyPlayer, spotifygtk_player, G_TYPE_OBJECT)

static void
on_state_fetched (SpotifyApi *api, JsonObject *result, GError *error, gpointer user_data)
{
  SpotifyPlayer *self = SPOTIFYGTK_PLAYER (user_data);
  if (error || !result) return;

  self->is_playing  = json_object_get_boolean_member_with_default (result, "is_playing", FALSE);
  self->position_ms = json_object_get_int_member_with_default     (result, "progress_ms", 0);

  JsonObject *device = json_object_has_member (result, "device")
    ? json_object_get_object_member (result, "device") : NULL;
  if (device)
    self->volume_pct = (gint) json_object_get_int_member_with_default (device, "volume_percent", 50);

  JsonObject *item = json_object_has_member (result, "item")
    ? json_object_get_object_member (result, "item") : NULL;
  if (item) {
    g_free (self->track_name);
    self->track_name  = g_strdup (json_object_get_string_member_with_default (item, "name", ""));
    self->duration_ms = json_object_get_int_member_with_default (item, "duration_ms", 0);

    JsonArray *artists = json_object_has_member (item, "artists")
      ? json_object_get_array_member (item, "artists") : NULL;
    if (artists && json_array_get_length (artists) > 0) {
      JsonObject *artist = json_array_get_object_element (artists, 0);
      g_free (self->artist_name);
      self->artist_name = g_strdup (json_object_get_string_member_with_default (artist, "name", ""));
    }

    JsonObject *album = json_object_has_member (item, "album")
      ? json_object_get_object_member (item, "album") : NULL;
    if (album) {
      JsonArray *images = json_object_has_member (album, "images")
        ? json_object_get_array_member (album, "images") : NULL;
      if (images && json_array_get_length (images) > 0) {
        guint pick = json_array_get_length (images) > 1 ? 1 : 0;
        JsonObject *img = json_array_get_object_element (images, pick);
        g_free (self->album_art_url);
        self->album_art_url = g_strdup (json_object_get_string_member_with_default (img, "url", ""));
      }
    }
  }

  g_signal_emit (self, signals[SIG_STATE_CHANGED], 0);
}

static gboolean
poll_cb (gpointer user_data)
{
  SpotifyPlayer *self = SPOTIFYGTK_PLAYER (user_data);
  spotifygtk_api_get_playback_state (self->api, on_state_fetched, self);
  return G_SOURCE_CONTINUE;
}

void spotifygtk_player_play_pause (SpotifyPlayer *self)
{
  if (self->is_playing) spotifygtk_api_pause (self->api, NULL, NULL);
  else                  spotifygtk_api_play  (self->api, NULL, NULL, NULL);
}

void spotifygtk_player_next     (SpotifyPlayer *self) { spotifygtk_api_next     (self->api, NULL, NULL); }
void spotifygtk_player_previous (SpotifyPlayer *self) { spotifygtk_api_previous (self->api, NULL, NULL); }

void spotifygtk_player_set_volume (SpotifyPlayer *self, gint percent)
{
  self->volume_pct = percent;
  spotifygtk_api_set_volume (self->api, percent, NULL, NULL);
}

void spotifygtk_player_seek (SpotifyPlayer *self, gint64 position_ms)
{
  self->position_ms = position_ms;
  spotifygtk_api_seek (self->api, position_ms, NULL, NULL);
}

gboolean     spotifygtk_player_is_playing      (SpotifyPlayer *self) { return self->is_playing; }
gint64       spotifygtk_player_get_position    (SpotifyPlayer *self) { return self->position_ms; }
gint64       spotifygtk_player_get_duration    (SpotifyPlayer *self) { return self->duration_ms; }
gint         spotifygtk_player_get_volume      (SpotifyPlayer *self) { return self->volume_pct; }
const gchar *spotifygtk_player_get_track_name  (SpotifyPlayer *self) { return self->track_name  ? self->track_name  : ""; }
const gchar *spotifygtk_player_get_artist_name (SpotifyPlayer *self) { return self->artist_name ? self->artist_name : ""; }
const gchar *spotifygtk_player_get_album_art_url (SpotifyPlayer *self) { return self->album_art_url ? self->album_art_url : ""; }

void
spotifygtk_player_start_polling (SpotifyPlayer *self, guint interval_ms)
{
  spotifygtk_player_stop_polling (self);
  self->poll_interval_ms = interval_ms;
  poll_cb (self);
  self->poll_source_id = g_timeout_add (interval_ms, poll_cb, self);
}

void
spotifygtk_player_stop_polling (SpotifyPlayer *self)
{
  if (self->poll_source_id) {
    g_source_remove (self->poll_source_id);
    self->poll_source_id = 0;
  }
}

static void
spotifygtk_player_dispose (GObject *object)
{
  SpotifyPlayer *self = SPOTIFYGTK_PLAYER (object);
  spotifygtk_player_stop_polling (self);
  g_clear_object (&self->api);
  G_OBJECT_CLASS (spotifygtk_player_parent_class)->dispose (object);
}

static void
spotifygtk_player_finalize (GObject *object)
{
  SpotifyPlayer *self = SPOTIFYGTK_PLAYER (object);
  g_free (self->track_name);
  g_free (self->artist_name);
  g_free (self->album_art_url);
  G_OBJECT_CLASS (spotifygtk_player_parent_class)->finalize (object);
}

static void
spotifygtk_player_class_init (SpotifyPlayerClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  oc->dispose  = spotifygtk_player_dispose;
  oc->finalize = spotifygtk_player_finalize;

  signals[SIG_STATE_CHANGED] =
    g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void spotifygtk_player_init (SpotifyPlayer *self) { (void) self; }

SpotifyPlayer *
spotifygtk_player_new (SpotifyApi *api)
{
  SpotifyPlayer *self = g_object_new (SPOTIFYGTK_TYPE_PLAYER, NULL);
  self->api = g_object_ref (api);
  return self;
}

/*
 * track_item.c — GObject wrapper for one track in a list model.
 */

#include "track_item.h"

struct _SpotifyGtkTrackItem {
  GObject parent_instance;

  SpotifyNativeTrack track;   /* owned copy */
  guint    number;
  gboolean playing;
  gboolean paused;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkTrackItem, spotifygtk_track_item, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
spotifygtk_track_item_finalize (GObject *object)
{
  SpotifyGtkTrackItem *self = SPOTIFYGTK_TRACK_ITEM (object);
  g_free (self->track.uri);
  g_free (self->track.name);
  g_free (self->track.artists);
  g_free (self->track.album);
  g_free (self->track.cover_id);
  G_OBJECT_CLASS (spotifygtk_track_item_parent_class)->finalize (object);
}

static void
spotifygtk_track_item_class_init (SpotifyGtkTrackItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = spotifygtk_track_item_finalize;

  signals[CHANGED] = g_signal_new ("changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void
spotifygtk_track_item_init (SpotifyGtkTrackItem *self)
{
  (void) self;
}

SpotifyGtkTrackItem *
spotifygtk_track_item_new (const SpotifyNativeTrack *track, guint number)
{
  SpotifyGtkTrackItem *self = g_object_new (SPOTIFYGTK_TYPE_TRACK_ITEM, NULL);
  self->number = number;
  self->track.uri         = g_strdup (track->uri);
  self->track.name        = g_strdup (track->name);
  self->track.artists     = g_strdup (track->artists);
  self->track.album       = g_strdup (track->album);
  self->track.cover_id    = g_strdup (track->cover_id);
  self->track.duration_ms = track->duration_ms;
  self->track.is_explicit = track->is_explicit;
  return self;
}

const SpotifyNativeTrack *
spotifygtk_track_item_get_track (SpotifyGtkTrackItem *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_TRACK_ITEM (self), NULL);
  return &self->track;
}

guint
spotifygtk_track_item_get_number (SpotifyGtkTrackItem *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_TRACK_ITEM (self), 0);
  return self->number;
}

const gchar *
spotifygtk_track_item_get_uri (SpotifyGtkTrackItem *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_TRACK_ITEM (self), NULL);
  return self->track.uri;
}

gboolean
spotifygtk_track_item_get_playing (SpotifyGtkTrackItem *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_TRACK_ITEM (self), FALSE);
  return self->playing;
}

gboolean
spotifygtk_track_item_get_paused (SpotifyGtkTrackItem *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_TRACK_ITEM (self), FALSE);
  return self->paused;
}

void
spotifygtk_track_item_set_playing (SpotifyGtkTrackItem *self,
                                   gboolean playing, gboolean paused)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ITEM (self));

  if (self->playing == playing && self->paused == paused)
    return;

  self->playing = playing;
  self->paused  = paused;
  g_signal_emit (self, signals[CHANGED], 0);
}

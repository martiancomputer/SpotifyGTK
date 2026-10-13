/*
 * track_item.c — GObject wrapper for one track in a list model.
 */

#include "track_item.h"

struct _SpotifyGtkTrackItem {
  GObject parent_instance;

  SpotifyNativeTrack track;   /* owned metadata; strings are ref-counted */
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
  g_clear_pointer (&self->track.uri, g_ref_string_release);
  g_clear_pointer (&self->track.name, g_ref_string_release);
  g_clear_pointer (&self->track.artists, g_ref_string_release);
  g_clear_pointer (&self->track.album, g_ref_string_release);
  g_clear_pointer (&self->track.cover_id, g_ref_string_release);
  g_clear_pointer (&self->track.cover_id_small, g_ref_string_release);
  g_clear_pointer (&self->track.album_uri, g_ref_string_release);
  g_clear_pointer (&self->track.artist_uri, g_ref_string_release);
  g_clear_pointer (&self->track.section_title, g_ref_string_release);
  g_clear_pointer (&self->track.section_detail, g_ref_string_release);
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
  g_return_val_if_fail (track != NULL, NULL);
  SpotifyGtkTrackItem *self = g_object_new (SPOTIFYGTK_TYPE_TRACK_ITEM, NULL);
  self->number = number;
  /* GTK can retain a focused item after a model splice, until focus is moved
   * during a later frame. Borrowing a page-owned track therefore outlives the
   * source on unlike, refresh or navigation. Own every field, and share equal
   * strings across replacement/filtered models instead of duplicating them
   * on each keystroke. Interned ref strings disappear with their last owner. */
  self->track.uri         = track->uri ? g_ref_string_new_intern (track->uri) : NULL;
  self->track.name        = track->name ? g_ref_string_new_intern (track->name) : NULL;
  self->track.artists     = track->artists ? g_ref_string_new_intern (track->artists) : NULL;
  self->track.album       = track->album ? g_ref_string_new_intern (track->album) : NULL;
  self->track.cover_id    = track->cover_id ? g_ref_string_new_intern (track->cover_id) : NULL;
  self->track.cover_id_small = track->cover_id_small ? g_ref_string_new_intern (track->cover_id_small) : NULL;
  self->track.album_uri   = track->album_uri ? g_ref_string_new_intern (track->album_uri) : NULL;
  self->track.artist_uri  = track->artist_uri ? g_ref_string_new_intern (track->artist_uri) : NULL;
  self->track.release_year = track->release_year;
  self->track.section_title = track->section_title ? g_ref_string_new_intern (track->section_title) : NULL;
  self->track.section_detail = track->section_detail ? g_ref_string_new_intern (track->section_detail) : NULL;
  self->track.duration_ms = track->duration_ms;
  self->track.liked_at = track->liked_at;
  self->track.device_index = track->device_index;
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
  const SpotifyNativeTrack *track = spotifygtk_track_item_get_track (self);
  return track ? track->uri : NULL;
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

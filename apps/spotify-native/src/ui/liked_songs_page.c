/*
 * liked_songs_page.c — Liked Songs page implementation.
 */

#include "liked_songs_page.h"
#include "track_list.h"

#include <string.h>

/* A real collection runs to thousands of tracks (4,773 on the account this
 * was developed against) and a single metadata batch that large is rejected
 * by the server. Track SPOTIFYGTK_SESSION_MAX_BATCH -- the largest batch
 * confirmed to resolve in one shot -- rather than repeating a number here that
 * then goes stale when the ceiling moves. Paging past it is still a follow-up:
 * a 4,773-track collection needs several batches, not a bigger one. */
#define LIKED_SONGS_LIMIT SPOTIFYGTK_SESSION_MAX_BATCH

/* After a failure, ignore refresh requests for this long, so revisiting the
 * page cannot turn one error into a stream of retries. */
#define RETRY_COOLDOWN_US (10 * G_USEC_PER_SEC)

struct _SpotifyGtkLikedSongsPage {
  GtkBox parent_instance;

  SpotifyGtkTrackList  *list;
  GtkSearchEntry       *filter_entry;
  SpotifyNativeSession *session;
  GCancellable         *in_flight;

  /* The full loaded collection, kept so the filter can rebuild the visible
   * list from it without re-fetching. Owned (a ref on the loaded array). */
  GPtrArray *all_tracks;

  gboolean loaded;
  gint64   retry_after;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkLikedSongsPage, spotifygtk_liked_songs_page, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
on_track_activated (SpotifyGtkTrackList *list, gpointer track, gpointer user_data)
{
  SpotifyGtkLikedSongsPage *self = user_data;
  g_signal_emit (self, signals[TRACK_ACTIVATED], 0, track);
  (void) list;
}

/* A track matches when every whitespace-separated term of the (casefolded)
 * query appears somewhere in its title, artists or album. The filter is local:
 * it narrows the already-loaded collection, so it is instant and works offline
 * of the catalog. */
static gboolean
track_matches (const SpotifyNativeTrack *t, const gchar *const *terms)
{
  g_autofree gchar *hay = g_strdup_printf ("%s\t%s\t%s",
    t->name ? t->name : "", t->artists ? t->artists : "", t->album ? t->album : "");
  g_autofree gchar *hay_cf = g_utf8_casefold (hay, -1);

  for (guint i = 0; terms[i]; i++)
    if (!strstr (hay_cf, terms[i]))    /* an empty term matches, which is fine */
      return FALSE;
  return TRUE;
}

/* Rebuild the visible list from all_tracks under the current filter text. */
static void
apply_filter (SpotifyGtkLikedSongsPage *self)
{
  if (!self->all_tracks)
    return;

  const gchar *query = gtk_editable_get_text (GTK_EDITABLE (self->filter_entry));

  if (!query || !*query) {
    spotifygtk_track_list_set_native_tracks (self->list, self->all_tracks);
    if (self->all_tracks->len == 0)
      spotifygtk_track_list_set_status (self->list, "No liked songs yet.");
    return;
  }

  g_autofree gchar *cf = g_utf8_casefold (query, -1);
  g_auto(GStrv) terms = g_strsplit (cf, " ", -1);

  /* Borrowed pointers into all_tracks; set_native_tracks takes its own copies,
   * so this shallow array needs no free func. */
  g_autoptr(GPtrArray) filtered = g_ptr_array_new ();
  for (guint i = 0; i < self->all_tracks->len; i++) {
    SpotifyNativeTrack *t = g_ptr_array_index (self->all_tracks, i);
    if (track_matches (t, (const gchar *const *) terms))
      g_ptr_array_add (filtered, t);
  }

  spotifygtk_track_list_set_native_tracks (self->list, filtered);
  if (filtered->len == 0)
    spotifygtk_track_list_set_status (self->list, "No matches.");
}

static void
on_filter_changed (GtkSearchEntry *entry, gpointer user_data)
{
  apply_filter (user_data);
  (void) entry;
}

static void
on_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  GWeakRef             *ref     = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(SpotifyGtkLikedSongsPage) self = g_weak_ref_get (ref);
  g_weak_ref_clear (ref);
  g_free (ref);

  g_autoptr(GPtrArray) tracks =
    spotifygtk_native_session_load_tracks_finish (session, result, &err);

  if (!self)
    return;

  g_clear_object (&self->in_flight);

  if (!tracks) {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;
    g_autofree gchar *msg = g_strdup_printf ("Couldn't load liked songs: %s", err->message);
    spotifygtk_track_list_clear (self->list);
    spotifygtk_track_list_set_status (self->list, msg);
    self->retry_after = g_get_monotonic_time () + RETRY_COOLDOWN_US;
    return;
  }

  self->retry_after = 0;
  g_clear_pointer (&self->all_tracks, g_ptr_array_unref);
  self->all_tracks = g_ptr_array_ref (tracks);
  apply_filter (self);   /* honours whatever is already typed (usually nothing) */

  self->loaded = TRUE;
}

static void
spotifygtk_liked_songs_page_dispose (GObject *object)
{
  SpotifyGtkLikedSongsPage *self = SPOTIFYGTK_LIKED_SONGS_PAGE (object);

  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  g_clear_object (&self->session);
  g_clear_pointer (&self->all_tracks, g_ptr_array_unref);

  G_OBJECT_CLASS (spotifygtk_liked_songs_page_parent_class)->dispose (object);
}

static void
spotifygtk_liked_songs_page_class_init (SpotifyGtkLikedSongsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_liked_songs_page_dispose;

  signals[TRACK_ACTIVATED] = g_signal_new ("track-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
spotifygtk_liked_songs_page_init (SpotifyGtkLikedSongsPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 24);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  GtkWidget *title = gtk_label_new ("Liked Songs");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (self), title);

  /* Local filter over the loaded collection — narrows what's shown, does not
   * hit the catalog. */
  self->filter_entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_search_entry_set_placeholder_text (self->filter_entry, "Filter liked songs");
  gtk_widget_set_halign (GTK_WIDGET (self->filter_entry), GTK_ALIGN_START);
  gtk_widget_set_size_request (GTK_WIDGET (self->filter_entry), 340, -1);
  g_signal_connect (self->filter_entry, "search-changed",
                    G_CALLBACK (on_filter_changed), self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->filter_entry));

  self->list = spotifygtk_track_list_new ();
  spotifygtk_track_list_set_numbered (self->list, TRUE);
  g_signal_connect (self->list, "track-activated", G_CALLBACK (on_track_activated), self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->list));
}

SpotifyGtkLikedSongsPage *
spotifygtk_liked_songs_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_LIKED_SONGS_PAGE, NULL);
}

void
spotifygtk_liked_songs_page_set_session (SpotifyGtkLikedSongsPage *self,
                                         SpotifyNativeSession     *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));

  g_clear_object (&self->session);
  self->session = session ? g_object_ref (session) : NULL;

  /* A newly-ready session is exactly what fixes an earlier failure. */
  self->loaded = FALSE;
  self->retry_after = 0;
}

void
spotifygtk_liked_songs_page_refresh (SpotifyGtkLikedSongsPage *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));

  if (self->loaded || self->in_flight)
    return;
  if (self->retry_after && g_get_monotonic_time () < self->retry_after)
    return;

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    spotifygtk_track_list_set_status (self->list, "Not signed in yet.");
    return;
  }

  g_autofree gchar *uri = spotifygtk_native_session_dup_collection_uri (self->session);
  if (!uri) {
    spotifygtk_track_list_set_status (self->list, "Username not resolved yet.");
    return;
  }

  spotifygtk_track_list_set_status (self->list, "Loading…");

  self->in_flight = g_cancellable_new ();

  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, self);

  spotifygtk_native_session_load_tracks (self->session, uri, LIKED_SONGS_LIMIT,
                                         self->in_flight, on_tracks_loaded, ref);
}

void
spotifygtk_liked_songs_page_set_playing_uri (SpotifyGtkLikedSongsPage *self, const gchar *uri, gboolean playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));
  spotifygtk_track_list_set_playing_uri (self->list, uri, playing);
}

SpotifyGtkTrackList *
spotifygtk_liked_songs_page_get_list (SpotifyGtkLikedSongsPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self), NULL);
  return self->list;
}

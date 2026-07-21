/*
 * search_page.c — Catalog search page implementation.
 *
 * Typing debounces for SEARCH_DEBOUNCE_MS before hitting the network, so a
 * fast typist produces one query rather than one per keystroke. Each
 * dispatched query carries a serial and its own GCancellable: the serial
 * drops results that a newer query has superseded, and the cancellable
 * stops the older request rather than merely ignoring its answer.
 */

#include "search_page.h"
#include "track_list.h"

#include "spotify/spclient.h"   /* build_search_uri */

#define SEARCH_DEBOUNCE_MS 350
#define SEARCH_RESULT_LIMIT 40

struct _SpotifyGtkSearchPage {
  GtkBox parent_instance;

  GtkSearchEntry      *entry;
  SpotifyGtkTrackList *results;

  SpotifyNativeSession *session;

  GCancellable *in_flight;
  guint         debounce_id;
  guint64       serial;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkSearchPage, spotifygtk_search_page, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct {
  GWeakRef page;
  guint64  serial;
} SearchClosure;

static void
on_track_activated (SpotifyGtkTrackList *list, gpointer track, gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  g_signal_emit (self, signals[TRACK_ACTIVATED], 0, track);
  (void) list;
}

static void
on_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  SearchClosure        *cl      = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(SpotifyGtkSearchPage) self = g_weak_ref_get (&cl->page);
  guint64 serial = cl->serial;
  g_weak_ref_clear (&cl->page);
  g_free (cl);

  g_autoptr(GPtrArray) tracks =
    spotifygtk_native_session_load_tracks_finish (session, result, &err);

  if (!self)
    return;                       /* page went away mid-request */
  if (serial != self->serial)
    return;                       /* superseded by a newer query */

  if (!tracks) {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;
    g_autofree gchar *msg = g_strdup_printf ("Search failed: %s", err->message);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_set_status (self->results, msg);
    return;
  }

  spotifygtk_track_list_set_native_tracks (self->results, tracks);
  if (tracks->len == 0)
    spotifygtk_track_list_set_status (self->results, "No results.");
}

static gboolean
dispatch_search (gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  self->debounce_id = 0;

  const gchar *query = gtk_editable_get_text (GTK_EDITABLE (self->entry));

  /* Any previous query is now obsolete: bump the serial so a late answer is
   * discarded, and cancel it so it stops occupying the session. */
  self->serial++;
  if (self->in_flight) {
    g_cancellable_cancel (self->in_flight);
    g_clear_object (&self->in_flight);
  }

  if (!query || !*query) {
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_set_status (self->results, NULL);
    return G_SOURCE_REMOVE;
  }

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    spotifygtk_track_list_set_status (self->results, "Not signed in yet.");
    return G_SOURCE_REMOVE;
  }

  g_autofree gchar *uri = spotifygtk_spclient_build_search_uri (query);
  if (!uri)
    return G_SOURCE_REMOVE;

  spotifygtk_track_list_set_status (self->results, "Searching…");

  self->in_flight = g_cancellable_new ();

  SearchClosure *cl = g_new0 (SearchClosure, 1);
  g_weak_ref_init (&cl->page, self);
  cl->serial = self->serial;

  spotifygtk_native_session_load_tracks (self->session, uri, SEARCH_RESULT_LIMIT,
                                         self->in_flight, on_tracks_loaded, cl);
  return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  g_clear_handle_id (&self->debounce_id, g_source_remove);
  self->debounce_id = g_timeout_add (SEARCH_DEBOUNCE_MS, dispatch_search, self);
  (void) entry;
}

static void
on_search_activate (GtkSearchEntry *entry, gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  g_clear_handle_id (&self->debounce_id, g_source_remove);
  dispatch_search (self);
  (void) entry;
}

static void
spotifygtk_search_page_dispose (GObject *object)
{
  SpotifyGtkSearchPage *self = SPOTIFYGTK_SEARCH_PAGE (object);

  g_clear_handle_id (&self->debounce_id, g_source_remove);
  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  g_clear_object (&self->session);

  G_OBJECT_CLASS (spotifygtk_search_page_parent_class)->dispose (object);
}

static void
spotifygtk_search_page_class_init (SpotifyGtkSearchPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_search_page_dispose;

  signals[TRACK_ACTIVATED] = g_signal_new ("track-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
spotifygtk_search_page_init (SpotifyGtkSearchPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 24);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  GtkWidget *title = gtk_label_new ("Search");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (self), title);

  self->entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->entry), 400, -1);
  gtk_widget_set_halign (GTK_WIDGET (self->entry), GTK_ALIGN_START);
  gtk_search_entry_set_placeholder_text (self->entry, "Songs, artists, albums");
  g_signal_connect (self->entry, "search-changed", G_CALLBACK (on_search_changed), self);
  g_signal_connect (self->entry, "activate", G_CALLBACK (on_search_activate), self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->entry));

  self->results = spotifygtk_track_list_new ();
  g_signal_connect (self->results, "track-activated", G_CALLBACK (on_track_activated), self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->results));
}

SpotifyGtkSearchPage *
spotifygtk_search_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_SEARCH_PAGE, NULL);
}

void
spotifygtk_search_page_set_session (SpotifyGtkSearchPage *self, SpotifyNativeSession *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self));

  g_clear_object (&self->session);
  self->session = session ? g_object_ref (session) : NULL;
}

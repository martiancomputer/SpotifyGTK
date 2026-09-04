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
#include "album_grid.h"

#include "spotify/spclient.h"   /* build_search_uri */

#define SEARCH_DEBOUNCE_MS 350
#define SEARCH_RESULT_LIMIT SPOTIFYGTK_SESSION_MAX_TRACKS

/* Height reserved above the first row for the floating header (title + entry
 * + its top/bottom margins). Rows scroll up under the header's fading edge. */
#define SEARCH_HEADER_INSET 128

struct _SpotifyGtkSearchPage {
  GtkBox parent_instance;

  GtkSearchEntry      *entry;
  SpotifyGtkTrackList *results;
  SpotifyGtkAlbumGrid *albums;
  GtkWidget           *albums_section;   /* "Albums" heading + shelf; hidden when empty */

  SpotifyNativeSession *session;

  GCancellable *in_flight;
  guint         debounce_id;
  gboolean      searching;
  guint64       serial;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkSearchPage, spotifygtk_search_page, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, LOADING_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct {
  GWeakRef page;
  guint64  serial;
} SearchClosure;

static void
set_searching (SpotifyGtkSearchPage *self, gboolean searching)
{
  searching = !!searching;
  if (self->searching == searching)
    return;

  self->searching = searching;
  g_signal_emit (self, signals[LOADING_CHANGED], 0, searching);
}

static void
on_track_activated (SpotifyGtkTrackList *list, gpointer track, gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  g_signal_emit (self, signals[TRACK_ACTIVATED], 0, track);
  (void) list;
}

/* The floating header must clear whichever section is topmost. When albums
 * show, they sit at the top and carry the inset (set once at build time), so
 * the list drops its own; when there are no albums, the list is topmost and
 * takes the inset back. */
static void
set_albums_visible (SpotifyGtkSearchPage *self, gboolean visible)
{
  gtk_widget_set_visible (self->albums_section, visible);
  spotifygtk_track_list_set_top_inset (self->results,
                                       visible ? 0 : SEARCH_HEADER_INSET);
  if (!visible)
    spotifygtk_album_grid_clear (self->albums);
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

  g_clear_object (&self->in_flight);
  set_searching (self, FALSE);

  if (!tracks) {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;
    g_autofree gchar *msg = g_strdup_printf ("Search failed: %s", err->message);
    set_albums_visible (self, FALSE);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_set_status (self->results, msg);
    return;
  }

  if (tracks->len == 0) {
    set_albums_visible (self, FALSE);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_set_status (self->results, "No results.");
    return;
  }

  /* These are already ranked catalog results from the desktop search query.
   * The old 20-track context needed a word-match filter because it contained
   * playback filler; applying that filter here would throw away Spotify's
   * fuzzy/semantic matches and make the expanded result set look small again. */
  g_autoptr(GPtrArray) shown = g_ptr_array_ref (tracks);

  /* The albums shelf is the distinct albums present in these very results --
   * real matches, grouped, not a second query. */
  guint n_albums = spotifygtk_album_grid_set_from_tracks (self->albums, shown, 40);
  set_albums_visible (self, n_albums > 0);

  spotifygtk_track_list_set_native_tracks (self->results, shown);
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
    set_searching (self, FALSE);
    set_albums_visible (self, FALSE);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_set_status (self->results, NULL);
    return G_SOURCE_REMOVE;
  }

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    set_searching (self, FALSE);
    spotifygtk_track_list_set_status (self->results, "Not signed in yet.");
    return G_SOURCE_REMOVE;
  }

  g_autofree gchar *uri = spotifygtk_spclient_build_search_uri (query);
  if (!uri) {
    set_searching (self, FALSE);
    return G_SOURCE_REMOVE;
  }

  /* Keep any previous results in place while their replacement arrives. The
   * progress strip floats above the page, so searching neither inserts a row
   * nor makes the content jump between list and status views. */
  spotifygtk_track_list_set_status (self->results, NULL);
  set_searching (self, TRUE);

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
  signals[LOADING_CHANGED] = g_signal_new ("loading-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
spotifygtk_search_page_init (SpotifyGtkSearchPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* The list fills the whole page and the header floats over its top edge, so
   * rows scroll up *underneath* the title and entry rather than starting below
   * them. The list is inset by the header's height so the first row clears it. */
  GtkWidget *overlay = gtk_overlay_new ();
  gtk_widget_set_hexpand (overlay, TRUE);
  gtk_widget_set_vexpand (overlay, TRUE);

  GtkWidget *base = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);

  /* Albums shelf: hidden until a search returns albums. It sits at the top and
   * carries the header inset so its cards slide under the frosted header; the
   * list below it then needs no inset of its own. */
  self->albums_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (self->albums_section, 35);
  gtk_widget_set_margin_end (self->albums_section, 12);
  gtk_widget_set_margin_top (self->albums_section, SEARCH_HEADER_INSET);
  gtk_widget_set_visible (self->albums_section, FALSE);

  GtkWidget *albums_heading = gtk_label_new ("Albums");
  gtk_widget_add_css_class (albums_heading, "section-heading");
  gtk_label_set_xalign (GTK_LABEL (albums_heading), 0.0);
  gtk_box_append (GTK_BOX (self->albums_section), albums_heading);

  self->albums = spotifygtk_album_grid_new_shelf ();
  gtk_box_append (GTK_BOX (self->albums_section), GTK_WIDGET (self->albums));
  gtk_box_append (GTK_BOX (base), self->albums_section);

  self->results = spotifygtk_track_list_new ();
  gtk_widget_set_margin_start (GTK_WIDGET (self->results), 35);
  gtk_widget_set_margin_end (GTK_WIDGET (self->results), 12);
  /* No bottom margin; see the note in liked_songs_page.c. */
  gtk_widget_set_vexpand (GTK_WIDGET (self->results), TRUE);
  spotifygtk_track_list_set_top_inset (self->results, SEARCH_HEADER_INSET);
  g_signal_connect (self->results, "track-activated", G_CALLBACK (on_track_activated), self);
  gtk_box_append (GTK_BOX (base), GTK_WIDGET (self->results));

  gtk_overlay_set_child (GTK_OVERLAY (overlay), base);

  /* Frosted header: pinned to the top, its own height only, so clicks below it
   * fall through to the list. `.search-glass` gives it the gradient fade. */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (header, "search-glass");
  gtk_widget_set_valign (header, GTK_ALIGN_START);
  gtk_widget_set_hexpand (header, TRUE);
  gtk_widget_set_margin_top (header, 24);
  gtk_widget_set_margin_bottom (header, 22);

  GtkWidget *title = gtk_label_new ("Search");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.5);
  gtk_box_append (GTK_BOX (header), title);

  self->entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->entry), 460, -1);
  gtk_widget_set_halign (GTK_WIDGET (self->entry), GTK_ALIGN_CENTER);
  gtk_search_entry_set_placeholder_text (self->entry, "Songs, artists, albums");
  g_signal_connect (self->entry, "search-changed", G_CALLBACK (on_search_changed), self);
  g_signal_connect (self->entry, "activate", G_CALLBACK (on_search_activate), self);
  gtk_box_append (GTK_BOX (header), GTK_WIDGET (self->entry));

  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), header);

  gtk_box_append (GTK_BOX (self), overlay);
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

void
spotifygtk_search_page_set_playing_uri (SpotifyGtkSearchPage *self, const gchar *uri, gboolean playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self));
  spotifygtk_track_list_set_playing_uri (self->results, uri, playing);
}

SpotifyGtkTrackList *
spotifygtk_search_page_get_list (SpotifyGtkSearchPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self), NULL);
  return self->results;
}

SpotifyGtkAlbumGrid *
spotifygtk_search_page_get_album_grid (SpotifyGtkSearchPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self), NULL);
  return self->albums;
}

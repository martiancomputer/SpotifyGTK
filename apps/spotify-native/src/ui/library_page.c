/*
 * library_page.c — Library page implementation.
 *
 * What is real here: the albums grid is the distinct albums present in the
 * signed-in user's Liked Songs, resolved through the same /context-resolve
 * path everything else uses. Those are genuinely albums the user has saved
 * tracks from, so the grid is real library data, not filler.
 *
 * Playlists used to be a footer here, saying they were unavailable. That cost
 * the grid a permanent row of albums to display one sentence, so it now lives
 * on its own page (see window.c) and this page is albums only.
 */

#include "library_page.h"
#include "album_grid.h"

struct _SpotifyGtkLibraryPage {
  GtkBox parent_instance;

  SpotifyGtkAlbumGrid  *albums;
  GtkWidget            *albums_status;   /* "Loading…" / empty note */
  GtkWidget            *header_revealer; /* title + heading, folds away on scroll */
  SpotifyNativeSession *session;         /* not owned */
  GCancellable         *load_cancel;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkLibraryPage, spotifygtk_library_page, GTK_TYPE_BOX)

static void
spotifygtk_library_page_dispose (GObject *object)
{
  SpotifyGtkLibraryPage *self = SPOTIFYGTK_LIBRARY_PAGE (object);
  if (self->load_cancel) {
    g_cancellable_cancel (self->load_cancel);
    g_clear_object (&self->load_cancel);
  }
  self->session = NULL;
  G_OBJECT_CLASS (spotifygtk_library_page_parent_class)->dispose (object);
}

static void
spotifygtk_library_page_class_init (SpotifyGtkLibraryPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_library_page_dispose;
}

/* === Loading albums from the collection === */

typedef struct { GWeakRef page; } LibLoad;

static void
on_albums_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LibLoad *cl = user_data;
  g_autoptr(SpotifyGtkLibraryPage) self = g_weak_ref_get (&cl->page);
  g_weak_ref_clear (&cl->page);
  g_free (cl);

  g_autoptr(GError) err = NULL;
  g_autoptr(GPtrArray) tracks = spotifygtk_native_session_load_tracks_finish (
    SPOTIFYGTK_NATIVE_SESSION (source), result, &err);

  if (!self)
    return;
  if (!tracks) {
    if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      gtk_label_set_text (GTK_LABEL (self->albums_status),
                          "Could not load your library right now.");
    return;
  }

  /* Show every distinct album in the collection -- there are far fewer albums
   * than the tracks they come from, so this cap is never the binding one. */
  guint n = spotifygtk_album_grid_set_from_tracks (self->albums, tracks,
                                                   SPOTIFYGTK_SESSION_MAX_BATCH);
  gtk_widget_set_visible (self->albums_status, n == 0);
  if (n == 0)
    gtk_label_set_text (GTK_LABEL (self->albums_status),
                        "No saved tracks to draw albums from yet.");
}

void
spotifygtk_library_page_set_session (SpotifyGtkLibraryPage *self,
                                     SpotifyNativeSession  *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));

  self->session = session;

  if (self->load_cancel) {
    g_cancellable_cancel (self->load_cancel);
    g_clear_object (&self->load_cancel);
  }
  if (!session ||
      spotifygtk_native_session_get_state (session) != SPOTIFYGTK_SESSION_READY)
    return;

  g_autofree gchar *uri = spotifygtk_native_session_dup_collection_uri (session);
  if (!uri)
    return;

  gtk_label_set_text (GTK_LABEL (self->albums_status), "Loading…");
  gtk_widget_set_visible (self->albums_status, TRUE);

  self->load_cancel = g_cancellable_new ();
  LibLoad *cl = g_new0 (LibLoad, 1);
  g_weak_ref_init (&cl->page, self);
  spotifygtk_native_session_load_tracks (session, uri, SPOTIFYGTK_SESSION_MAX_BATCH,
                                         self->load_cancel,
                                         on_albums_loaded, cl);
}

SpotifyGtkAlbumGrid *
spotifygtk_library_page_get_album_grid (SpotifyGtkLibraryPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self), NULL);
  return self->albums;
}


/*
 * Fold the page header away as the grid scrolls.
 *
 * The obvious implementation -- put the header and the grid in one outer
 * GtkScrolledWindow -- cannot be used here. That hands the GridView unbounded
 * height, so it stops virtualising and realises every album at once, which is
 * the several-hundred-card, ~75MB texture problem album_grid.c was rewritten to
 * fix. So the grid stays the only scrolling region and the header reacts to its
 * adjustment instead.
 *
 * Two thresholds rather than one: folding the header changes how much height
 * the grid gets, which moves the adjustment, which could re-trigger the test.
 * A single threshold would sit on that boundary and flap. Hiding well after the
 * point where it reappears gives the hysteresis to settle.
 */
#define HEADER_HIDE_AT   90.0
#define HEADER_SHOW_AT   12.0

static void
on_albums_scrolled (GtkAdjustment *adj, gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;
  gdouble value = gtk_adjustment_get_value (adj);
  gboolean revealed =
    gtk_revealer_get_reveal_child (GTK_REVEALER (self->header_revealer));

  if (revealed && value > HEADER_HIDE_AT)
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_revealer), FALSE);
  else if (!revealed && value < HEADER_SHOW_AT)
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_revealer), TRUE);
}

/* === Building blocks === */

static GtkWidget *
heading (const gchar *text)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_widget_add_css_class (label, "section-heading");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_margin_top (label, 8);
  return label;
}

static void
spotifygtk_library_page_init (SpotifyGtkLibraryPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* No outer scroller: the albums GridView brings its own, and nesting it in a
   * second vertical scroller would hand it unbounded height, so it would
   * realise every album at once instead of only the visible ones. The grid is
   * the one scrolling region; this header sits above it and folds away in
   * response to its adjustment -- see on_albums_scrolled(). */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (header, 35);
  gtk_widget_set_margin_end (header, 35);
  gtk_widget_set_margin_top (header, 24);

  GtkWidget *title = gtk_label_new ("Library");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (header), title);

  /* --- Albums (real, from the collection) --- */
  gtk_box_append (GTK_BOX (header), heading ("Albums"));

  self->albums_status = gtk_label_new ("Not signed in yet.");
  gtk_widget_add_css_class (self->albums_status, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (self->albums_status), 0.0);
  gtk_box_append (GTK_BOX (header), self->albums_status);

  self->header_revealer = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (self->header_revealer),
                                    GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_transition_duration (GTK_REVEALER (self->header_revealer), 180);
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_revealer), TRUE);
  gtk_revealer_set_child (GTK_REVEALER (self->header_revealer), header);
  gtk_box_append (GTK_BOX (self), self->header_revealer);

  self->albums = spotifygtk_album_grid_new_grid ();
  /*
   * The horizontal inset goes on the cards, not on this widget. A margin here
   * would push the scrollbar inward too, stacking its width on top of the
   * margin and leaving a dead gutter beside it -- the bar belongs flush with
   * the page edge like every other scroller in the app. The end inset is a
   * little smaller than the start because the bar itself occupies the
   * difference, which is what makes the two sides read as equal.
   */
  spotifygtk_album_grid_set_content_margins (self->albums, 35, 22);
  gtk_widget_set_margin_top (GTK_WIDGET (self->albums), 12);
  gtk_widget_set_vexpand (GTK_WIDGET (self->albums), TRUE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->albums));

  GtkAdjustment *vadj = spotifygtk_album_grid_get_vadjustment (self->albums);
  if (vadj)
    g_signal_connect (vadj, "value-changed", G_CALLBACK (on_albums_scrolled), self);

}

SpotifyGtkLibraryPage *
spotifygtk_library_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_LIBRARY_PAGE, NULL);
}

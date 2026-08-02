/*
 * library_page.c — Library page implementation.
 *
 * What is real here: the albums grid is the distinct albums present in the
 * signed-in user's Liked Songs, resolved through the same /context-resolve
 * path everything else uses. Those are genuinely albums the user has saved
 * tracks from, so the grid is real library data, not filler.
 *
 * What is still missing: the *playlist* list needs spclient's rootlist
 * endpoint, which returns playlist4_external protobuf rather than the JSON the
 * catalog path parses. Until that parser exists the page says so rather than
 * inventing playlists. (A playlist opens fine once its URI is known — the
 * context-resolve path already handles spotify:playlist:<id>.)
 */

#include "library_page.h"
#include "album_grid.h"

struct _SpotifyGtkLibraryPage {
  GtkBox parent_instance;

  SpotifyGtkAlbumGrid  *albums;
  GtkWidget            *albums_status;   /* "Loading…" / empty note */
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

  /* Show every distinct album in the collection (there are far fewer albums
   * than the 1000 tracks they come from). */
  guint n = spotifygtk_album_grid_set_from_tracks (self->albums, tracks, 1000);
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
  spotifygtk_native_session_load_tracks (session, uri, 1000, self->load_cancel,
                                         on_albums_loaded, cl);
}

SpotifyGtkAlbumGrid *
spotifygtk_library_page_get_album_grid (SpotifyGtkLibraryPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self), NULL);
  return self->albums;
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
   * realise every album at once instead of only the visible ones. The headings
   * stay fixed and the grid is the one scrolling region. */
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

  gtk_box_append (GTK_BOX (self), header);

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

  /* --- Playlists (honest gap) --- */
  /*
   * This sits outside the scrolling grid, so every pixel it occupies is taken
   * from the albums permanently. It was costing roughly 90px -- heading margin,
   * an 8px gap, the note, and 24px beneath -- to say one sentence, which left a
   * band of empty page under the note and cut the last album row mid-card.
   * Tightened to about half that: still legible, still honest about the gap,
   * without charging the grid a full album row for the privilege.
   */
  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start (footer, 35);
  gtk_widget_set_margin_end (footer, 35);
  gtk_widget_set_margin_top (footer, 4);
  gtk_widget_set_margin_bottom (footer, 10);
  GtkWidget *pl_heading = heading ("Playlists");
  gtk_widget_set_margin_top (pl_heading, 0);
  gtk_box_append (GTK_BOX (footer), pl_heading);

  GtkWidget *note = gtk_label_new (
    "Your playlists aren’t available in this client yet.");
  gtk_widget_set_tooltip_text (note,
    "Needs spclient’s rootlist endpoint, which returns playlist4_external "
    "protobuf — a larger schema than the track metadata the rest of the "
    "catalog uses, and the one remaining piece of this migration. A playlist "
    "still opens once its URI is known: the context-resolve path handles "
    "spotify:playlist:<id> like everything else.");
  gtk_widget_add_css_class (note, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (note), 0.0);
  gtk_label_set_wrap (GTK_LABEL (note), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (note), 74);
  gtk_box_append (GTK_BOX (footer), note);

  gtk_box_append (GTK_BOX (self), footer);
}

SpotifyGtkLibraryPage *
spotifygtk_library_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_LIBRARY_PAGE, NULL);
}

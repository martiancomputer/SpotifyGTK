/*
 * home_page.c — Home page implementation.
 *
 * Laid out to the reference design: a greeting header with quick actions,
 * then three sections — a horizontal card shelf, a two-column recent list,
 * and a second shelf.
 *
 * The sections are built but empty, and say so. None of the three has a
 * data source on the native stack yet: listening history has no known
 * spclient endpoint (librespot exposes none), and the playlist rootlist
 * returns playlist4_external protobuf, which is unparsed. Populating them
 * with invented albums to match the mockup would make the page look
 * finished while showing things the user does not actually have.
 */

#include "home_page.h"
#include "album_grid.h"

struct _SpotifyGtkHomePage {
  GtkBox parent_instance;
  GtkLabel *greeting;

  /* "From your Liked Songs": the one shelf with a real native data source.
   * Populated from the collection once the session is ready; the other
   * sections still have no endpoint and say so. */
  SpotifyGtkAlbumGrid  *albums;
  GtkWidget            *liked_section;
  SpotifyNativeSession *session;      /* not owned; the window outlives us */
  GCancellable         *load_cancel;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkHomePage, spotifygtk_home_page, GTK_TYPE_BOX)

static void
spotifygtk_home_page_dispose (GObject *object)
{
  SpotifyGtkHomePage *self = SPOTIFYGTK_HOME_PAGE (object);
  if (self->load_cancel) {
    g_cancellable_cancel (self->load_cancel);
    g_clear_object (&self->load_cancel);
  }
  self->session = NULL;
  G_OBJECT_CLASS (spotifygtk_home_page_parent_class)->dispose (object);
}

static void
spotifygtk_home_page_class_init (SpotifyGtkHomePageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_home_page_dispose;
}

/* === Loading the Liked Songs shelf === */

typedef struct { GWeakRef page; } HomeLoad;

static void
on_liked_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  HomeLoad *cl = user_data;
  g_autoptr(SpotifyGtkHomePage) self = g_weak_ref_get (&cl->page);
  g_weak_ref_clear (&cl->page);
  g_free (cl);

  g_autoptr(GError) err = NULL;
  g_autoptr(GPtrArray) tracks = spotifygtk_native_session_load_tracks_finish (
    SPOTIFYGTK_NATIVE_SESSION (source), result, &err);

  if (!self)
    return;
  if (!tracks) {
    /* Cancelled or failed: leave the section hidden rather than showing an
     * error on the home page. Search and Liked Songs still work. */
    return;
  }

  /* A shelf, not the whole library: 100 cards is already a long scroll, and
   * the Library page shows the full album set. */
  guint n = spotifygtk_album_grid_set_from_tracks (self->albums, tracks, 100);
  gtk_widget_set_visible (self->liked_section, n > 0);
}

void
spotifygtk_home_page_set_session (SpotifyGtkHomePage   *self,
                                  SpotifyNativeSession *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_HOME_PAGE (self));

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

  self->load_cancel = g_cancellable_new ();
  HomeLoad *cl = g_new0 (HomeLoad, 1);
  g_weak_ref_init (&cl->page, self);
  spotifygtk_native_session_load_tracks (session, uri, 1000, self->load_cancel,
                                         on_liked_loaded, cl);
}

SpotifyGtkAlbumGrid *
spotifygtk_home_page_get_album_grid (SpotifyGtkHomePage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_HOME_PAGE (self), NULL);
  return self->albums;
}

static const gchar *
greeting_for_hour (gint hour)
{
  if (hour < 12) return "Good morning.";
  if (hour < 18) return "Good afternoon.";
  return "Good evening.";
}

/* Section header: title on the left, optional action widget on the right. */
static GtkWidget *
build_section_header (const gchar *title, GtkWidget *action)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_bottom (row, 12);

  GtkWidget *label = gtk_label_new (title);
  gtk_widget_add_css_class (label, "section-heading");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (row), label);

  if (action) {
    gtk_widget_set_valign (action, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (row), action);
  }

  return row;
}

/* Placeholder body for a section with no data source yet. Deliberately
 * plain: it should read as "not built" rather than "failed to load". */
static GtkWidget *
build_empty_state (const gchar *summary, const gchar *technical)
{
  /* A dim line, not a filled card. Three of these stacked as padded slabs made
   * the one section with real data look like the exception on the page. The
   * short line says what the user needs to know; the protocol reason -- which
   * is developer-facing, however true -- moves to the tooltip. */
  GtkWidget *label = gtk_label_new (summary);
  gtk_widget_add_css_class (label, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 76);
  gtk_widget_set_margin_bottom (label, 8);
  if (technical)
    gtk_widget_set_tooltip_text (label, technical);

  return label;
}


static void
spotifygtk_home_page_init (SpotifyGtkHomePage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 28);
  gtk_widget_set_margin_start (content, 34);
  gtk_widget_set_margin_end (content, 24);
  gtk_widget_set_margin_top (content, 22);
  gtk_widget_set_margin_bottom (content, 24);

  /* --- Header: title + greeting, with quick actions on the right --- */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);

  GtkWidget *titles = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand (titles, TRUE);

  GtkWidget *title = gtk_label_new ("Home");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (titles), title);

  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  self->greeting = GTK_LABEL (gtk_label_new (greeting_for_hour (g_date_time_get_hour (now))));
  gtk_widget_add_css_class (GTK_WIDGET (self->greeting), "greeting");
  gtk_label_set_xalign (self->greeting, 0.0);
  gtk_box_append (GTK_BOX (titles), GTK_WIDGET (self->greeting));

  gtk_box_append (GTK_BOX (header), titles);

  /* No per-page Notifications/Settings icons here: the sidebar's Settings
   * item is the single, working entry point, and a second gear on the page
   * (whose signal the window never even connected) only duplicated it. */

  gtk_box_append (GTK_BOX (content), header);

  /* --- From your Liked Songs (real data) --- */
  self->liked_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (self->liked_section),
                  build_section_header ("From your Liked Songs", NULL));
  self->albums = spotifygtk_album_grid_new_shelf ();
  gtk_box_append (GTK_BOX (self->liked_section), GTK_WIDGET (self->albums));
  gtk_widget_set_visible (self->liked_section, FALSE);   /* until the load lands */
  gtk_box_append (GTK_BOX (content), self->liked_section);

  /* --- Continue Listening --- */
  GtkWidget *continue_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (continue_section),
                  build_section_header ("Continue Listening", NULL));
  gtk_box_append (GTK_BOX (continue_section),
                  build_empty_state ("Nothing here yet — your playlists aren’t available "
                                     "in this client so far.",
                                     "Needs spclient’s rootlist endpoint, which returns "
                                     "playlist4_external protobuf rather than the JSON the "
                                     "rest of the catalog uses."));
  gtk_box_append (GTK_BOX (content), continue_section);

  /* --- Recently Played --- */
  GtkWidget *recent_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (recent_section),
                  build_section_header ("Recently Played", NULL));
  gtk_box_append (GTK_BOX (recent_section),
                  build_empty_state ("Your listening history isn’t available in this client.",
                                     "No spclient endpoint for recently-played is known; "
                                     "librespot exposes none, so there is nothing to "
                                     "port here yet."));
  gtk_box_append (GTK_BOX (content), recent_section);

  /* --- Made For You --- */
  GtkWidget *made_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (made_section), build_section_header ("Made For You", NULL));
  gtk_box_append (GTK_BOX (made_section),
                  build_empty_state ("Personalised mixes aren’t available in this client. "
                                     "Search and Liked Songs work today.",
                                     "Editorial mixes come from Spotify’s recommendation "
                                     "endpoints, which this client does not talk to yet."));
  gtk_box_append (GTK_BOX (content), made_section);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), content);
  gtk_box_append (GTK_BOX (self), scroller);
}

SpotifyGtkHomePage *
spotifygtk_home_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_HOME_PAGE, NULL);
}

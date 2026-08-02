/*
 * context_page.c — Album/artist page backed by the native session.
 *
 * See context_page.h. A thin wrapper over SpotifyGtkTrackList: it owns the
 * header and the load lifecycle, and forwards everything about rows to the
 * list, so the row context menu and play-context behave identically to the
 * search and liked-songs pages.
 */

#include "context_page.h"

#define CONTEXT_PAGE_LIMIT 200

struct _SpotifyGtkContextPage {
  GtkBox parent_instance;

  GtkLabel            *kind_label;
  GtkLabel            *title_label;
  GtkLabel            *year_label;
  SpotifyGtkTrackList *list;

  SpotifyNativeSession *session;
  GCancellable         *in_flight;
  gchar                *current_uri;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkContextPage, spotifygtk_context_page, GTK_TYPE_BOX)

static void
on_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  GWeakRef             *ref     = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(SpotifyGtkContextPage) self = g_weak_ref_get (ref);
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
    g_autofree gchar *msg = g_strdup_printf ("Couldn't load: %s", err->message);
    spotifygtk_track_list_clear (self->list);
    spotifygtk_track_list_set_status (self->list, msg);
    /* A failed load must not be remembered as the current URI, or a retry via
     * re-navigation would be swallowed by the same-URI no-op. */
    g_clear_pointer (&self->current_uri, g_free);
    return;
  }

  /*
   * The release year rides on the tracks rather than arriving separately --
   * context-resolve returns a track list, not an album object, so this is the
   * only place it is available. Every track on an album carries the same year;
   * take the first that has one, since a compilation can carry stragglers with
   * none. A mixed context (a playlist) will show its first track's year, which
   * is why the caller only asks for this on albums.
   */
  for (guint i = 0; i < tracks->len; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (tracks, i);
    if (t->release_year > 0) {
      g_autofree gchar *year = g_strdup_printf ("%d", t->release_year);
      gtk_label_set_text (self->year_label, year);
      break;
    }
  }

  spotifygtk_track_list_set_native_tracks (self->list, tracks);
  if (tracks->len == 0)
    spotifygtk_track_list_set_status (self->list, "Nothing here.");
}

static void
spotifygtk_context_page_dispose (GObject *object)
{
  SpotifyGtkContextPage *self = SPOTIFYGTK_CONTEXT_PAGE (object);

  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  g_clear_object (&self->session);
  g_clear_pointer (&self->current_uri, g_free);

  G_OBJECT_CLASS (spotifygtk_context_page_parent_class)->dispose (object);
}

static void
spotifygtk_context_page_class_init (SpotifyGtkContextPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_context_page_dispose;
}

static void
spotifygtk_context_page_init (SpotifyGtkContextPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 4);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 24);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  self->kind_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->kind_label), "dim-text");
  gtk_label_set_xalign (self->kind_label, 0.0);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->kind_label));

  /* Title and release year share a row, year trailing. The title expands so
   * the year stays pinned to the right edge however long the title is, and
   * the title -- not the year -- is what ellipsises when space runs out. */
  GtkWidget *title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_bottom (title_row, 8);

  self->title_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->title_label), "title-text");
  gtk_label_set_xalign (self->title_label, 0.0);
  gtk_label_set_ellipsize (self->title_label, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (GTK_WIDGET (self->title_label), TRUE);
  gtk_box_append (GTK_BOX (title_row), GTK_WIDGET (self->title_label));

  self->year_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->year_label), "dim-text");
  gtk_label_set_xalign (self->year_label, 1.0);
  gtk_widget_set_valign (GTK_WIDGET (self->year_label), GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->year_label), 6);
  gtk_box_append (GTK_BOX (title_row), GTK_WIDGET (self->year_label));

  gtk_box_append (GTK_BOX (self), title_row);

  self->list = spotifygtk_track_list_new ();
  spotifygtk_track_list_set_numbered (self->list, TRUE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->list));
}

SpotifyGtkContextPage *
spotifygtk_context_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_CONTEXT_PAGE, NULL);
}

void
spotifygtk_context_page_set_session (SpotifyGtkContextPage *self,
                                     SpotifyNativeSession  *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));

  g_set_object (&self->session, session);
}

void
spotifygtk_context_page_load (SpotifyGtkContextPage *self,
                              const gchar           *uri,
                              const gchar           *title,
                              const gchar           *kind)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));
  if (!uri || !*uri)
    return;

  gtk_label_set_text (self->kind_label, kind ? kind : "");
  gtk_label_set_text (self->title_label, title ? title : "");
  /* Cleared here rather than left stale: the year belongs to the previous
   * context until this one's tracks come back with their own. */
  gtk_label_set_text (self->year_label, "");

  /* Already showing this exactly — don't re-fetch on a repeat navigation. */
  if (g_strcmp0 (uri, self->current_uri) == 0 && !self->in_flight)
    return;

  if (self->in_flight) {
    g_cancellable_cancel (self->in_flight);
    g_clear_object (&self->in_flight);
  }

  g_free (self->current_uri);
  self->current_uri = g_strdup (uri);

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    spotifygtk_track_list_clear (self->list);
    spotifygtk_track_list_set_status (self->list, "Not signed in yet.");
    g_clear_pointer (&self->current_uri, g_free);
    return;
  }

  spotifygtk_track_list_clear (self->list);
  spotifygtk_track_list_set_status (self->list, "Loading…");

  self->in_flight = g_cancellable_new ();

  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, self);

  spotifygtk_native_session_load_tracks (self->session, uri, CONTEXT_PAGE_LIMIT,
                                         self->in_flight, on_tracks_loaded, ref);
}

SpotifyGtkTrackList *
spotifygtk_context_page_get_list (SpotifyGtkContextPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self), NULL);
  return self->list;
}

void
spotifygtk_context_page_set_playing_uri (SpotifyGtkContextPage *self,
                                         const gchar *uri, gboolean playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));
  spotifygtk_track_list_set_playing_uri (self->list, uri, playing);
}

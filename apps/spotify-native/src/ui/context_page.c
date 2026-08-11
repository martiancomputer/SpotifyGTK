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

static void on_action_clicked (GtkButton *button, gpointer user_data);
static gboolean align_action_to_durations (GtkWidget *w, GdkFrameClock *clock,
                                           gpointer data);

struct _SpotifyGtkContextPage {
  GtkBox parent_instance;

  GtkLabel            *kind_label;
  GtkLabel            *title_label;

  /* Save an album, or drop a playlist from the library. One button, because
   * the page is one page and only ever shows one of the two. */
  GtkWidget           *action_btn;
  GtkWidget           *title_row;
  gchar               *current_kind;
  SpotifyGtkContextActionFunc action_fn;
  gpointer                    action_data;
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
  g_clear_pointer (&self->current_kind, g_free);

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

  /*
   * Title and release year share a row, the year sitting just past the end of
   * the title rather than out at the right margin. Pinned to the edge it was
   * a long eye-track away from the thing it describes, and on a wide window it
   * read as an unrelated element.
   *
   * So the title does *not* expand -- a trailing spacer absorbs the slack
   * instead, which keeps the year adjacent whatever the title's length. The
   * title still ellipsises when the row runs out of room, because an
   * ellipsising label has a small minimum width and yields first.
   */
  GtkWidget *title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_bottom (title_row, 8);

  self->title_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->title_label), "title-text");
  gtk_label_set_xalign (self->title_label, 0.0);
  gtk_label_set_ellipsize (self->title_label, PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (title_row), GTK_WIDGET (self->title_label));

  self->year_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->year_label), "dim-text");
  gtk_label_set_xalign (self->year_label, 0.0);
  /* Bottom-aligned against a much larger title, so it settles near the
   * baseline instead of floating beside the cap height. */
  gtk_widget_set_valign (GTK_WIDGET (self->year_label), GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->year_label), 6);
  gtk_box_append (GTK_BOX (title_row), GTK_WIDGET (self->year_label));

  GtkWidget *title_slack = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (title_slack, TRUE);
  gtk_box_append (GTK_BOX (title_row), title_slack);

  self->action_btn = gtk_button_new_with_label ("");
  gtk_widget_add_css_class (self->action_btn, "flat");
  gtk_widget_set_valign (self->action_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->action_btn, FALSE);
  g_signal_connect (self->action_btn, "clicked",
                    G_CALLBACK (on_action_clicked), self);
  gtk_box_append (GTK_BOX (title_row), self->action_btn);
  self->title_row = title_row;

  gtk_box_append (GTK_BOX (self), title_row);

  self->list = spotifygtk_track_list_new ();
  spotifygtk_track_list_set_numbered (self->list, TRUE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->list));

  gtk_widget_add_tick_callback (GTK_WIDGET (self), align_action_to_durations,
                                self, NULL);
}

/*
 * Keep the action button's right edge on the duration column.
 *
 * The durations are inset by the list's scrollbar and the row's own padding;
 * the header is not, so left alone the button overhangs them. Measured from a
 * laid-out row rather than guessed -- the same lesson as the artist page,
 * where the gutter turned out to be 44px against an assumed dozen.
 */
static gboolean
align_action_to_durations (GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
  SpotifyGtkContextPage *self = data;
  (void) w; (void) clock;

  if (!self->title_row || !self->list)
    return G_SOURCE_REMOVE;

  gdouble dur_right = 0;
  if (!spotifygtk_track_list_duration_edge (self->list, GTK_WIDGET (self), &dur_right))
    return G_SOURCE_CONTINUE;   /* no row laid out yet */

  graphene_rect_t page;
  if (!gtk_widget_compute_bounds (GTK_WIDGET (self), GTK_WIDGET (self), &page))
    return G_SOURCE_CONTINUE;

  gint want = (gint) (page.size.width - dur_right);
  if (want < 0)
    want = 0;
  gtk_widget_set_margin_end (self->title_row, want);

  /*
   * Once is enough. What is being measured is the *inset* from the page's
   * right edge, and the durations are right-aligned, so the inset does not
   * change with the window's width -- only the absolute position does. A tick
   * that stayed armed would cost a bounds computation every frame forever, on
   * a page whose scroll performance was just paid for.
   */
  return G_SOURCE_REMOVE;
}

static void
on_action_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkContextPage *self = user_data;
  (void) button;
  if (self->action_fn && self->current_uri)
    self->action_fn (self->current_uri, self->current_kind, self->action_data);
}

void
spotifygtk_context_page_set_action_handler (SpotifyGtkContextPage      *self,
                                            SpotifyGtkContextActionFunc fn,
                                            gpointer                    user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));
  self->action_fn   = fn;
  self->action_data = user_data;
}

void
spotifygtk_context_page_set_action (SpotifyGtkContextPage *self,
                                    const gchar *label, gboolean visible,
                                    gboolean destructive)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));
  if (!self->action_btn)
    return;
  gtk_button_set_label (GTK_BUTTON (self->action_btn), label ? label : "");
  gtk_widget_set_visible (self->action_btn, visible);

  /* Colour on hover only, and only for the one that destroys something. */
  if (destructive)
    gtk_widget_add_css_class (self->action_btn, "destructive-hover");
  else
    gtk_widget_remove_css_class (self->action_btn, "destructive-hover");
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
  g_free (self->current_kind);
  self->current_kind = g_strdup (kind);

  gtk_widget_add_tick_callback (GTK_WIDGET (self), align_action_to_durations,
                                self, NULL);

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

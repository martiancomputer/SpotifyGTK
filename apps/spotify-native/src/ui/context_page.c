/*
 * context_page.c — Album/artist page backed by the native session.
 *
 * See context_page.h. A thin wrapper over SpotifyGtkTrackList: it owns the
 * header and the load lifecycle, and forwards everything about rows to the
 * list, so the row context menu and play-context behave identically to the
 * search and liked-songs pages.
 */

#include "context_page.h"
#include "../log_file.h"
#include "cover_loader.h"
#include "settings.h"
#include "smooth_scroll.h"
#include <string.h>

/* Metadata is cheap and ordered; covers remain strictly viewport-loaded.
 * Load the full supported context so the count/duration are not a 200-song
 * preview presented as a playlist total. */
#define CONTEXT_PAGE_LIMIT SPOTIFYGTK_SESSION_MAX_TRACKS
#define CONTEXT_COVER_PX 256

static void on_action_clicked (GtkButton *button, gpointer user_data);

struct _SpotifyGtkContextPage {
  GtkBox parent_instance;

  GtkLabel            *kind_label;
  GtkLabel            *title_label;

  /* Save an album, or drop a playlist from the library. One button, because
   * the page is one page and only ever shows one of the two. */
  GtkWidget           *action_btn;
  GtkWidget           *title_row;
  GtkWidget           *compact_header, *expanded_header, *expanded_action;
  GtkLabel            *expanded_kind, *expanded_title, *metadata;
  GtkPicture          *cover;
  GCancellable        *cover_request;
  gchar               *cover_id;
  gboolean             compact;
  gchar               *current_kind;
  SpotifyGtkContextActionFunc action_fn;
  gpointer                    action_data;
  GtkLabel            *year_label;
  SpotifyGtkTrackList *list;
  GtkScrolledWindow  *scroller;

  SpotifyNativeSession *session;
  GCancellable         *in_flight;
  gchar                *current_uri;
  guint                 generation;
  gboolean              loading;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkContextPage, spotifygtk_context_page, GTK_TYPE_BOX)

enum { LOADING_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct {
  GWeakRef page;
  guint    generation;
} ContextLoad;

static void
release_header_cover (SpotifyGtkContextPage *self)
{
  if (self->cover_request) g_cancellable_cancel (self->cover_request);
  g_clear_object (&self->cover_request);
  if (self->cover) gtk_picture_set_paintable (self->cover, NULL);
}

static void
on_header_cover_loaded (GdkTexture *texture, gpointer data)
{
  SpotifyGtkContextPage *self = data;
  g_clear_object (&self->cover_request);
  gtk_picture_set_paintable (self->cover, texture ? GDK_PAINTABLE (texture) : NULL);
}

static void
request_header_cover (SpotifyGtkContextPage *self)
{
  if (self->compact || !gtk_widget_get_mapped (GTK_WIDGET (self)) ||
      spotifygtk_settings_get_media_mode (spotifygtk_settings_get_default ()) != SPOTIFYGTK_MEDIA_FULL ||
      !self->cover_id || self->cover_request || gtk_picture_get_paintable (self->cover))
    return;
  /* The header scrolls with the page. Retain at most one header cover, and
   * don't decode it while the user is far down the track list. */
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (self->scroller);
  if (gtk_adjustment_get_value (adjustment) >
      24 + gtk_widget_get_height (self->expanded_header) + 64)
    return;
  self->cover_request = g_cancellable_new ();
  spotifygtk_cover_load (self->cover_id,
    CONTEXT_COVER_PX * gtk_widget_get_scale_factor (GTK_WIDGET (self)), self->cover_request,
    on_header_cover_loaded, self);
}

static void
on_header_viewport_changed (GtkAdjustment *adjustment, gpointer data)
{
  SpotifyGtkContextPage *self = data;
  if (gtk_adjustment_get_value (adjustment) >
      24 + gtk_widget_get_height (self->expanded_header) + 64) {
    if (self->cover_request || gtk_picture_get_paintable (self->cover))
      release_header_cover (self);
  } else {
    request_header_cover (self);
  }
}

static void
on_page_map (GtkWidget *widget, gpointer data)
{
  (void) data;
  request_header_cover (SPOTIFYGTK_CONTEXT_PAGE (widget));
}

void
spotifygtk_context_page_set_header_cover (SpotifyGtkContextPage *self,
                                         const gchar *uri, const gchar *cover_id)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));
  if (g_strcmp0 (self->current_uri, uri) || !cover_id || !*cover_id ||
      g_strcmp0 (self->cover_id, cover_id) == 0) return;
  release_header_cover (self);
  g_free (self->cover_id);
  self->cover_id = g_strdup (cover_id);
  request_header_cover (self);
}

static void
on_page_unmap (GtkWidget *widget, gpointer data)
{
  (void) data;
  release_header_cover (SPOTIFYGTK_CONTEXT_PAGE (widget));
}

static void
on_layout_changed (SpotifyGtkSettings *settings, gpointer data)
{
  SpotifyGtkContextPage *self = data;
  gboolean compact = spotifygtk_settings_get_compact_mode (settings);
  if (self->compact != compact) {
    self->compact = compact;
    gtk_widget_set_visible (self->compact_header, compact);
    gtk_widget_set_visible (self->expanded_header, !compact);
    if (self->list) spotifygtk_track_list_set_show_covers (self->list, compact);
  }
  if (compact || spotifygtk_settings_get_media_mode (settings) != SPOTIFYGTK_MEDIA_FULL)
    release_header_cover (self);
  else request_header_cover (self);
}

static void
set_loading (SpotifyGtkContextPage *self, gboolean loading)
{
  loading = !!loading;
  if (self->loading == loading)
    return;
  self->loading = loading;
  g_signal_emit (self, signals[LOADING_CHANGED], 0, loading);
}

static void
on_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  ContextLoad          *cl      = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(SpotifyGtkContextPage) self = g_weak_ref_get (&cl->page);
  guint generation = cl->generation;
  g_weak_ref_clear (&cl->page);
  g_free (cl);

  g_autoptr(GPtrArray) tracks =
    spotifygtk_native_session_load_tracks_finish (session, result, &err);
  spotifygtk_runtime_schedule_heap_trim ();

  if (!self)
    return;
  if (generation != self->generation)
    return;

  g_clear_object (&self->in_flight);
  set_loading (self, FALSE);

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
   * none. Never present a playlist song's release year as the playlist's year.
   */
  gint year = 0;
  gint64 duration = 0;
  const gchar *artist = NULL;
  gboolean multiple = FALSE;
  gboolean album = g_str_has_prefix (self->current_uri, "spotify:album:");
  for (guint i = 0; i < tracks->len; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (tracks, i);
    duration += MAX (0, t->duration_ms);
    if (album && !year && t->release_year > 0) year = t->release_year;
    if (t->artists && *t->artists) {
      if (!artist) artist = t->artists;
      else if (g_strcmp0 (artist, t->artists) != 0) multiple = TRUE;
      if (strstr (t->artists, ", ")) multiple = TRUE;
    }
    if (!self->cover_id && t->cover_id && *t->cover_id)
      self->cover_id = g_strdup (t->cover_id);
  }
  g_autofree gchar *year_text = year ? g_strdup_printf ("%d", year) : g_strdup ("");
  gtk_label_set_text (self->year_label, year_text);
  gint64 minutes = duration / 60000;
  g_autofree gchar *time = minutes >= 60
    ? g_strdup_printf ("%" G_GINT64_FORMAT " hr %" G_GINT64_FORMAT " min", minutes / 60, minutes % 60)
    : g_strdup_printf ("%" G_GINT64_FORMAT " min", minutes);
  g_autofree gchar *summary = g_strdup_printf ("%s%s%s · %u %s · %s",
    year_text, year ? " · " : "", multiple ? "Multiple artists" : artist ? artist : "Unknown artist",
    tracks->len, tracks->len == CONTEXT_PAGE_LIMIT ? "tracks loaded" : tracks->len == 1 ? "track" : "tracks", time);
  gtk_label_set_text (self->metadata, summary);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->metadata), summary);
  request_header_cover (self);

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
  release_header_cover (self);
  g_clear_pointer (&self->cover_id, g_free);
  self->cover = NULL;
  g_clear_object (&self->session);
  g_clear_pointer (&self->current_uri, g_free);
  g_clear_pointer (&self->current_kind, g_free);
  if (self->scroller) {
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (self->scroller);
    g_signal_handlers_disconnect_by_data (adjustment, self);
    self->scroller = NULL;
  }

  G_OBJECT_CLASS (spotifygtk_context_page_parent_class)->dispose (object);
}

#ifdef SPOTIFYGTK_UI_TESTS
/* Exercise the actual completion/render path without auth, network, or a
 * second running application. This seam is absent from production builds. */
void
spotifygtk_context_page_test_complete (SpotifyGtkContextPage *self,
                                      const gchar *uri, GPtrArray *tracks)
{
  g_free (self->current_uri);
  self->current_uri = g_strdup (uri);
  self->generation++;
  g_autoptr(SpotifyNativeSession) session = spotifygtk_native_session_new ();
  g_autoptr(GTask) task = g_task_new (session, NULL, NULL, NULL);
  g_task_return_pointer (task, g_ptr_array_ref (tracks), (GDestroyNotify) g_ptr_array_unref);
  ContextLoad *cl = g_new0 (ContextLoad, 1);
  g_weak_ref_init (&cl->page, self);
  cl->generation = self->generation;
  on_tracks_loaded (G_OBJECT (session), G_ASYNC_RESULT (task), cl);
}
#endif

static void
spotifygtk_context_page_class_init (SpotifyGtkContextPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_context_page_dispose;
  signals[LOADING_CHANGED] = g_signal_new ("loading-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
spotifygtk_context_page_init (SpotifyGtkContextPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 4);
  /*
   * No bottom margin. It was 24px, and it cost twice: a dead band above the
   * playback bar, and a viewport shortened by that much so the last row was
   * cut wherever the new edge fell. Liked Songs and Search had the same margin
   * removed for the same reason; this page kept it and so kept the stray
   * rectangle after they lost it.
   */
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* One page-level viewport owns header and tracks. The embedded list is
   * inline and observes this viewport for bounded artwork overscan. */
  self->scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (self->scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (self->scroller, FALSE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->scroller), TRUE);
  spotifygtk_smooth_scroll_attach (self->scroller, GTK_ORIENTATION_VERTICAL);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start (content, 35);
  gtk_widget_set_margin_end (content, 12);
  gtk_widget_set_margin_top (content, 24);
  gtk_scrolled_window_set_child (self->scroller, content);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->scroller));

  self->kind_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->kind_label), "dim-text");
  gtk_label_set_xalign (self->kind_label, 0.0);
  self->compact_header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append (GTK_BOX (self->compact_header), GTK_WIDGET (self->kind_label));

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

  gtk_widget_set_margin_end (title_row, 24);
  gtk_box_append (GTK_BOX (self->compact_header), title_row);
  gtk_box_append (GTK_BOX (content), self->compact_header);

  self->expanded_header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 20);
  gtk_widget_set_margin_end (self->expanded_header, 24);
#if ADW_CHECK_VERSION(1, 7, 0)
  GtkWidget *hero = adw_wrap_box_new ();
  adw_wrap_box_set_child_spacing (ADW_WRAP_BOX (hero), 24);
  adw_wrap_box_set_line_spacing (ADW_WRAP_BOX (hero), 20);
  adw_wrap_box_set_wrap_policy (ADW_WRAP_BOX (hero), ADW_WRAP_MINIMUM);
#else
  /* The explicit allow-old-GTK build keeps a usable stacked header. */
  GtkWidget *hero = gtk_box_new (GTK_ORIENTATION_VERTICAL, 20);
#endif
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_size_request (text, 220, -1);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  self->expanded_kind = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->expanded_kind), "dim-text");
  gtk_label_set_xalign (self->expanded_kind, 0);
  self->expanded_title = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->expanded_title), "context-hero-title");
  gtk_label_set_xalign (self->expanded_title, 0);
  gtk_label_set_ellipsize (self->expanded_title, PANGO_ELLIPSIZE_END);
  gtk_label_set_wrap (self->expanded_title, TRUE);
  gtk_label_set_wrap_mode (self->expanded_title, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines (self->expanded_title, 2);
  gtk_box_append (GTK_BOX (text), GTK_WIDGET (self->expanded_kind));
  gtk_box_append (GTK_BOX (text), GTK_WIDGET (self->expanded_title));
  /* Same square cover-fit and clipped 12px art-large corners as Now Playing. */
  GtkWidget *frame = gtk_aspect_frame_new (0.5, 0.5, 1.0, FALSE);
  gtk_widget_set_size_request (frame, CONTEXT_COVER_PX, CONTEXT_COVER_PX);
  gtk_widget_set_halign (frame, GTK_ALIGN_START);
  gtk_widget_set_valign (frame, GTK_ALIGN_CENTER);
  GtkWidget *clip = gtk_overlay_new ();
  gtk_widget_add_css_class (clip, "art-large");
  gtk_widget_set_overflow (clip, GTK_OVERFLOW_HIDDEN);
  self->cover = GTK_PICTURE (gtk_picture_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->cover), "art-large");
  gtk_picture_set_content_fit (self->cover, GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink (self->cover, TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (clip), GTK_WIDGET (self->cover));
  gtk_aspect_frame_set_child (GTK_ASPECT_FRAME (frame), clip);
#if ADW_CHECK_VERSION(1, 7, 0)
  adw_wrap_box_append (ADW_WRAP_BOX (hero), frame);
  adw_wrap_box_append (ADW_WRAP_BOX (hero), text);
#else
  gtk_box_append (GTK_BOX (hero), frame);
  gtk_box_append (GTK_BOX (hero), text);
#endif
  gtk_box_append (GTK_BOX (self->expanded_header), hero);
  GtkWidget *meta_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  self->expanded_action = gtk_button_new_with_label ("");
  gtk_widget_add_css_class (self->expanded_action, "flat");
  gtk_widget_set_visible (self->expanded_action, FALSE);
  g_signal_connect (self->expanded_action, "clicked", G_CALLBACK (on_action_clicked), self);
  gtk_box_append (GTK_BOX (meta_row), self->expanded_action);
  self->metadata = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (self->metadata, 0);
  gtk_label_set_ellipsize (self->metadata, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (GTK_WIDGET (self->metadata), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->metadata), "dim-text");
  gtk_box_append (GTK_BOX (meta_row), GTK_WIDGET (self->metadata));
  gtk_box_append (GTK_BOX (text), meta_row);
  gtk_box_append (GTK_BOX (self->expanded_header), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_widget_set_margin_bottom (self->expanded_header, 12);
  gtk_box_append (GTK_BOX (content), self->expanded_header);

  self->list = spotifygtk_track_list_new ();
  spotifygtk_track_list_set_numbered (self->list, TRUE);
  spotifygtk_track_list_set_inline (self->list, TRUE);
  spotifygtk_track_list_set_external_viewport (self->list, self->scroller);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->list));
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (self->scroller);
  g_signal_connect (adjustment, "value-changed", G_CALLBACK (on_header_viewport_changed), self);
  g_signal_connect (adjustment, "changed", G_CALLBACK (on_header_viewport_changed), self);

  self->compact = !spotifygtk_settings_get_compact_mode (spotifygtk_settings_get_default ());
  on_layout_changed (spotifygtk_settings_get_default (), self);
  g_signal_connect_object (spotifygtk_settings_get_default (), "changed",
    G_CALLBACK (on_layout_changed), self, 0);
  g_signal_connect (self, "map", G_CALLBACK (on_page_map), NULL);
  g_signal_connect (self, "unmap", G_CALLBACK (on_page_unmap), NULL);
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
  GtkWidget *buttons[] = { self->action_btn, self->expanded_action };
  for (guint i = 0; i < G_N_ELEMENTS (buttons); i++) {
    gtk_button_set_label (GTK_BUTTON (buttons[i]), label ? label : "");
    gtk_widget_set_visible (buttons[i], visible);
    if (destructive) gtk_widget_add_css_class (buttons[i], "destructive-hover");
    else gtk_widget_remove_css_class (buttons[i], "destructive-hover");
  }
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

  if (self->session == session)
    return;

  self->generation++;
  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  set_loading (self, FALSE);
  g_set_object (&self->session, session);
  g_clear_pointer (&self->current_uri, g_free);
}

void
spotifygtk_context_page_load (SpotifyGtkContextPage *self,
                              const gchar           *uri,
                              const gchar           *title,
                              const gchar           *kind)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));

  g_free (self->current_kind);
  self->current_kind = g_strdup (kind);

  if (!uri || !*uri)
    return;

  gtk_label_set_text (self->kind_label, kind ? kind : "");
  gtk_label_set_text (self->title_label, title ? title : "");
  gtk_label_set_text (self->expanded_title, title ? title : "");
  gtk_label_set_text (self->expanded_kind, kind ? kind : "");
  /* Retain metadata and pending work on repeat navigation. */
  if (g_strcmp0 (uri, self->current_uri) == 0) return;
  self->generation++;
  release_header_cover (self);
  g_clear_pointer (&self->cover_id, g_free);
  gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (self->scroller), 0);
  gtk_label_set_text (self->metadata, "Loading details…");
  /* Cleared here rather than left stale: the year belongs to the previous
   * context until this one's tracks come back with their own. */
  gtk_label_set_text (self->year_label, "");

  if (self->in_flight) {
    self->generation++;
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
    set_loading (self, FALSE);
    return;
  }

  spotifygtk_track_list_clear (self->list);
  spotifygtk_track_list_set_status (self->list, NULL);
  set_loading (self, TRUE);

  self->in_flight = g_cancellable_new ();

  ContextLoad *cl = g_new0 (ContextLoad, 1);
  g_weak_ref_init (&cl->page, self);
  cl->generation = self->generation;

  spotifygtk_native_session_load_tracks (self->session, uri, CONTEXT_PAGE_LIMIT,
                                         self->in_flight, on_tracks_loaded, cl);
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

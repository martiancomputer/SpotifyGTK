/*
 * context_page.c — Album/artist page backed by the native session.
 *
 * See context_page.h. A thin wrapper over SpotifyGtkTrackList: it owns the
 * header and the load lifecycle, and forwards everything about rows to the
 * list, so the row context menu and play-context behave identically to the
 * search and liked-songs pages.
 */

#include "context_page.h"
#include "../spotify/catalog_cache.h"
#include "../spotify/catalog_snapshot.h"
#include "cover_loader.h"
#include "artwork_viewer.h"
#include "settings.h"
#include "local_catalog.h"
#include "../log_file.h"

#define CONTEXT_PAGE_LIMIT 200
/* With the existing 24px top inset and 16px header gap, this places the
 * album divider level with (but separate from) the sidebar divider. */
#define CONTEXT_HERO_COVER_PX 266
#define CONTEXT_BROWSE_CACHE_LIMIT 16

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
  guint                align_tick;   /* 0 when none is pending */
  guint                align_attempts;
  gchar               *current_kind;
  SpotifyGtkContextActionFunc action_fn;
  gpointer                    action_data;
  GtkLabel            *year_label;
  GtkWidget           *page_header; /* borrowed from the track list */
  GtkWidget           *compact_header;
  GtkWidget           *expanded_header;
  GtkLabel            *expanded_title;
  GtkLabel            *expanded_kind;
  GtkLabel            *expanded_meta;
  GtkPicture          *expanded_cover;
  GtkWidget           *expanded_action_btn;
  GtkWidget           *expanded_play_btn;
  GCancellable        *cover_request;
  SpotifyGtkTrackList *list;
  SpotifyGtkLocalSnapshot *local_snapshot; /* keeps borrowed list rows alive */
  SpotifyGtkDevicePlaylists *device_playlists; /* borrowed from window */
  GHashTable *cached_contexts; /* visited Spotify URI -> owned track snapshot */
  GQueue     *cache_order;     /* owned URI strings, oldest first */

  SpotifyNativeSession *session;
  GCancellable         *in_flight;
  gchar                *current_uri;
  gchar                *context_cover_id; /* the playlist's own art, not a track's */
  gchar                *hero_cover_id;    /* owned; requested only in artwork view */
  guint                 generation;
  gboolean              loading;
  gboolean              layout_initialized;
  gboolean              compact;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkContextPage, spotifygtk_context_page, GTK_TYPE_BOX)

enum { LOADING_CHANGED, PLAY_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct {
  GWeakRef page;
  guint    generation;
} ContextLoad;

static void
on_cover_loaded (GdkTexture *texture, gpointer user_data)
{
  SpotifyGtkContextPage *self = user_data;
  /* A failed full-size request must not erase a correct thumbnail preview. */
  if (texture) {
    gtk_picture_set_paintable (self->expanded_cover, GDK_PAINTABLE (texture));
    gtk_widget_set_visible (GTK_WIDGET (self->expanded_cover), TRUE);
  }
}

static void
show_hero_preview (SpotifyGtkContextPage *self, GdkTexture *preview)
{
  if (self->compact || !preview ||
      gtk_picture_get_paintable (self->expanded_cover))
    return;
  gtk_picture_set_paintable (self->expanded_cover, GDK_PAINTABLE (preview));
  gtk_widget_set_visible (GTK_WIDGET (self->expanded_cover), TRUE);
}

static void
on_hero_clicked (GtkButton *button, SpotifyGtkContextPage *self)
{
  spotifygtk_artwork_viewer_present (GTK_WIDGET (self), self->hero_cover_id,
                                     gtk_label_get_text (self->expanded_title));
  (void) button;
}

static void
refresh_hero_cover (SpotifyGtkContextPage *self)
{
  if (self->cover_request)
    g_cancellable_cancel (self->cover_request);
  g_clear_object (&self->cover_request);
  gtk_picture_set_paintable (self->expanded_cover, NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->expanded_cover), FALSE);

  /* A hidden hero must not own a texture or start a decode. The track list's
   * own overscan remains responsible for compact-row artwork. */
  if (self->compact || !self->hero_cover_id)
    return;

  self->cover_request = g_cancellable_new ();
  spotifygtk_cover_load (self->hero_cover_id,
                         320 * MAX (1, gtk_widget_get_scale_factor (GTK_WIDGET (self))),
                         self->cover_request, on_cover_loaded, self);
}

static void
on_layout_changed (SpotifyGtkSettings *settings,
                   SpotifyGtkContextPage *self)
{
  gboolean compact = spotifygtk_settings_get_compact_mode (settings);
  if (self->layout_initialized && self->compact == compact)
    return;
  self->layout_initialized = TRUE;
  self->compact = compact;
  /* The list owns the outer inset in both views. Compact keeps its original
   * 35/12 spacing; the artwork view mirrors the 35px left inset on the right.
   * A second inset on the header shifted its cover and divider independently
   * of the rows, so the header contributes no horizontal inset when expanded. */
  spotifygtk_track_list_set_content_margins (self->list, 35,
                                             compact ? 12 : 35);
  gtk_widget_set_margin_start (self->page_header, 0);
  gtk_widget_set_margin_end (self->page_header, compact ? 12 : 0);
  gtk_widget_set_margin_top (self->page_header, compact ? 4 : 24);
  gtk_widget_set_margin_bottom (self->page_header, compact ? 0 : 12);
  gtk_widget_set_visible (self->compact_header, compact);
  gtk_widget_set_visible (self->expanded_header, !compact);
  spotifygtk_track_list_set_show_album (self->list, compact);
  spotifygtk_track_list_set_show_cover (self->list, compact);
  spotifygtk_track_list_set_numbered (self->list, TRUE);
  refresh_hero_cover (self);
  if (!compact && self->align_tick != 0) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->align_tick);
    self->align_tick = 0;
  } else if (compact && self->align_tick == 0 && !self->loading && self->list &&
             gtk_widget_get_visible (self->action_btn)) {
    self->align_attempts = 0;
    self->align_tick = gtk_widget_add_tick_callback (
      GTK_WIDGET (self), align_action_to_durations, self, NULL);
  }
}

static void
update_expanded_metadata (SpotifyGtkContextPage *self, GPtrArray *tracks)
{
  gint year = 0;
  gint64 total_ms = 0;
  const gchar *artist = NULL;
  gboolean multiple = FALSE;
  for (guint i = 0; i < tracks->len; i++) {
    const SpotifyNativeTrack *track = g_ptr_array_index (tracks, i);
    if (!year && track->release_year > 0) year = track->release_year;
    if (!artist) artist = track->artists;
    else if (g_strcmp0 (artist, track->artists) != 0) multiple = TRUE;
    total_ms += MAX (track->duration_ms, 0);
  }
  if (year > 0 && (g_str_has_prefix (self->current_uri, "spotify:album:") ||
                   g_str_has_prefix (self->current_uri, "local:album:"))) {
    g_autofree gchar *text = g_strdup_printf ("%d", year);
    gtk_label_set_text (self->year_label, text);
  }
  const gchar *credit = multiple ? "Multiple artists" :
    (artist && *artist ? artist : "Unknown artist");
  guint hours = (guint) (total_ms / 3600000);
  guint minutes = (guint) ((total_ms / 60000) % 60);
  g_autofree gchar *meta = year > 0 &&
    (g_str_has_prefix (self->current_uri, "spotify:album:") ||
     g_str_has_prefix (self->current_uri, "local:album:"))
    ? g_strdup_printf ("%d · %s · %u tracks · %u hr %u min",
                       year, credit, tracks->len, hours, minutes)
    : g_strdup_printf ("%s · %u tracks · %u hr %u min",
                       credit, tracks->len, hours, minutes);
  gtk_label_set_text (self->expanded_meta, meta);

  /* An album's tracks carry its cover. A playlist's tracks do not: use the
   * cover known by the card/navigation source, never the first song's art. */
  const gchar *cover_id = self->context_cover_id;
  if (g_str_has_prefix (self->current_uri, "spotify:album:") ||
      g_str_has_prefix (self->current_uri, "local:album:")) {
    /* Navigation cards intentionally use shelf-size images. Album metadata
     * keeps the largest source: use that for the hero and expanded viewer,
     * not the smaller source inherited from the clicked Home/Library card. */
    const gchar *largest = NULL;
    for (guint i = 0; i < tracks->len && !largest; i++) {
      const SpotifyNativeTrack *track = g_ptr_array_index (tracks, i);
      if (track->cover_id && *track->cover_id) largest = track->cover_id;
    }
    if (largest) cover_id = largest;
  }
  if (g_strcmp0 (self->hero_cover_id, cover_id) != 0) {
    g_free (self->hero_cover_id);
    self->hero_cover_id = g_strdup (cover_id);
    refresh_hero_cover (self);
  }
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
remember_context (SpotifyGtkContextPage *self, const gchar *uri,
                  GPtrArray *tracks)
{
  if (!uri || !g_str_has_prefix (uri, "spotify:") || !tracks)
    return;
  GPtrArray *copy = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_track_free);
  for (guint i = 0; i < tracks->len; i++)
    g_ptr_array_add (copy, spotifygtk_native_track_copy (
      g_ptr_array_index (tracks, i)));
  for (GList *node = self->cache_order->head; node; node = node->next)
    if (g_strcmp0 (node->data, uri) == 0) {
      g_free (node->data);
      g_queue_delete_link (self->cache_order, node);
      break;
    }
  g_queue_push_tail (self->cache_order, g_strdup (uri));
  g_hash_table_replace (self->cached_contexts, g_strdup (uri), copy);
  while (self->cache_order->length > CONTEXT_BROWSE_CACHE_LIMIT) {
    gchar *oldest = g_queue_pop_head (self->cache_order);
    g_hash_table_remove (self->cached_contexts, oldest);
    g_free (oldest);
  }
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
    if (self->align_tick) {
      gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->align_tick);
      self->align_tick = 0;
    }
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
  g_autoptr(GPtrArray) visible = self->current_uri &&
    g_str_has_prefix (self->current_uri, "spotify:playlist:")
      ? spotifygtk_device_playlists_tracks (self->device_playlists,
                                            self->current_uri, tracks)
      : g_ptr_array_ref (tracks);
  /* Cache only the server sequence. The device overlay is independent and
   * must be applied fresh when browsing this playlist while signed out. */
  remember_context (self, self->current_uri, tracks);
  update_expanded_metadata (self, visible);
  spotifygtk_track_list_set_native_tracks (self->list, visible);
  /* The list may have been showing a different release while the async load
   * was pending. Reset after the model splice too, so GtkListView does not
   * restore the old item's scroll anchor into this new release. */
  spotifygtk_track_list_scroll_to_top (self->list);
  if (visible->len > 0 && self->compact && self->align_tick == 0 &&
      gtk_widget_get_visible (self->action_btn)) {
    self->align_attempts = 0;
    self->align_tick = gtk_widget_add_tick_callback (
      GTK_WIDGET (self), align_action_to_durations, self, NULL);
  }
  if (visible->len == 0)
    spotifygtk_track_list_set_status (self->list, "Nothing here.");
  if (visible->len == 0 && self->align_tick) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->align_tick);
    self->align_tick = 0;
  }
}

static void
spotifygtk_context_page_dispose (GObject *object)
{
  SpotifyGtkContextPage *self = SPOTIFYGTK_CONTEXT_PAGE (object);

  if (self->align_tick) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->align_tick);
    self->align_tick = 0;
  }
  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  if (self->cover_request)
    g_cancellable_cancel (self->cover_request);
  g_clear_object (&self->cover_request);
  g_signal_handlers_disconnect_by_data (spotifygtk_settings_get_default (), self);
  /* The list's borrowed rows must be removed before their snapshot. */
  if (self->local_snapshot)
    spotifygtk_track_list_clear (self->list);
  spotifygtk_local_snapshot_unref (self->local_snapshot);
  self->local_snapshot = NULL;
  g_clear_object (&self->session);
  g_clear_pointer (&self->current_uri, g_free);
  g_clear_pointer (&self->context_cover_id, g_free);
  g_clear_pointer (&self->hero_cover_id, g_free);
  g_clear_pointer (&self->cached_contexts, g_hash_table_unref);
  if (self->cache_order) {
    g_queue_free_full (self->cache_order, g_free);
    self->cache_order = NULL;
  }
  g_clear_pointer (&self->current_kind, g_free);

  G_OBJECT_CLASS (spotifygtk_context_page_parent_class)->dispose (object);
}

static void
spotifygtk_context_page_class_init (SpotifyGtkContextPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_context_page_dispose;
  signals[LOADING_CHANGED] = g_signal_new ("loading-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[PLAY_REQUESTED] = g_signal_new ("play-requested",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void
on_expanded_play_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkContextPage *self = user_data;
  g_signal_emit (self, signals[PLAY_REQUESTED], 0);
  (void) button;
}

static void
spotifygtk_context_page_init (SpotifyGtkContextPage *self)
{
  self->cached_contexts = g_hash_table_new_full (
    g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  self->cache_order = g_queue_new ();
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  self->page_header = header;
  gtk_widget_set_margin_start (header, 35);
  gtk_widget_set_margin_end (header, 12);
  gtk_widget_set_margin_top (header, 24);
  gtk_widget_set_margin_bottom (header, 12);
  gtk_widget_set_hexpand (header, TRUE);
  self->compact_header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append (GTK_BOX (header), self->compact_header);

  self->kind_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->kind_label), "dim-text");
  gtk_label_set_xalign (self->kind_label, 0.0);
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

  gtk_box_append (GTK_BOX (self->compact_header), title_row);

  self->expanded_header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  GtkWidget *hero = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  GtkWidget *cover_frame = gtk_overlay_new ();
  gtk_widget_set_size_request (cover_frame, CONTEXT_HERO_COVER_PX,
                              CONTEXT_HERO_COVER_PX);
  GtkWidget *background = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (background, "art-large");
  gtk_overlay_set_child (GTK_OVERLAY (cover_frame), background);
  self->expanded_cover = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (self->expanded_cover, GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink (self->expanded_cover, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->expanded_cover), "art-large");
  gtk_widget_set_visible (GTK_WIDGET (self->expanded_cover), FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (cover_frame), GTK_WIDGET (self->expanded_cover));
  GtkWidget *cover_button = gtk_button_new ();
  gtk_widget_add_css_class (cover_button, "artwork-open");
  gtk_widget_set_tooltip_text (cover_button, "View artwork");
  gtk_button_set_child (GTK_BUTTON (cover_button), cover_frame);
  g_signal_connect (cover_button, "clicked", G_CALLBACK (on_hero_clicked), self);
  gtk_box_append (GTK_BOX (hero), cover_button);
  GtkWidget *hero_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_hexpand (hero_text, TRUE);
  gtk_widget_set_valign (hero_text, GTK_ALIGN_CENTER);
  self->expanded_kind = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->expanded_kind), "dim-text");
  gtk_label_set_xalign (self->expanded_kind, 0);
  gtk_box_append (GTK_BOX (hero_text), GTK_WIDGET (self->expanded_kind));
  self->expanded_title = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->expanded_title), "title-text");
  gtk_label_set_xalign (self->expanded_title, 0);
  gtk_label_set_ellipsize (self->expanded_title, PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (hero_text), GTK_WIDGET (self->expanded_title));

  /* Keep the action and release details on one baseline below the title.
   * The metadata yields its width first, so neither long credits nor a narrow
   * window can push the cover or the action out of the page. */
  GtkWidget *meta_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  self->expanded_play_btn = gtk_button_new_with_label ("Play");
  gtk_widget_add_css_class (self->expanded_play_btn, "context-play");
  gtk_widget_set_valign (self->expanded_play_btn, GTK_ALIGN_CENTER);
  g_signal_connect (self->expanded_play_btn, "clicked",
                    G_CALLBACK (on_expanded_play_clicked), self);
  gtk_box_append (GTK_BOX (meta_row), self->expanded_play_btn);
  self->expanded_action_btn = gtk_button_new_with_label ("");
  gtk_widget_add_css_class (self->expanded_action_btn, "flat");
  gtk_widget_set_halign (self->expanded_action_btn, GTK_ALIGN_START);
  gtk_widget_set_valign (self->expanded_action_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->expanded_action_btn, FALSE);
  g_signal_connect (self->expanded_action_btn, "clicked",
                    G_CALLBACK (on_action_clicked), self);
  gtk_box_append (GTK_BOX (meta_row), self->expanded_action_btn);
  self->expanded_meta = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->expanded_meta), "dim-text");
  gtk_label_set_xalign (self->expanded_meta, 0);
  gtk_label_set_ellipsize (self->expanded_meta, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (GTK_WIDGET (self->expanded_meta), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (self->expanded_meta), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (meta_row), GTK_WIDGET (self->expanded_meta));
  gtk_box_append (GTK_BOX (hero_text), meta_row);
  gtk_box_append (GTK_BOX (hero), hero_text);
  gtk_box_append (GTK_BOX (self->expanded_header), hero);
  GtkWidget *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append (GTK_BOX (self->expanded_header), separator);
  gtk_box_append (GTK_BOX (header), self->expanded_header);

  self->list = spotifygtk_track_list_new ();
  spotifygtk_track_list_set_numbered (self->list, TRUE);
  spotifygtk_track_list_set_page_header (self->list, header);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->list));
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  g_signal_connect (settings, "changed", G_CALLBACK (on_layout_changed), self);
  on_layout_changed (settings, self);

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

  /* A row may never map (empty/error page, or navigation away during load).
   * Do not keep the application's frame clock active indefinitely for a
   * cosmetic alignment; a later successful load arms a fresh attempt. */
  if (++self->align_attempts > 120) {
    self->align_tick = 0;
    return G_SOURCE_REMOVE;
  }

  if (!self->title_row || !self->list) {
    self->align_tick = 0;
    return G_SOURCE_REMOVE;
  }
  if (!gtk_widget_get_visible (self->action_btn)) {
    self->align_tick = 0;
    return G_SOURCE_REMOVE;
  }

  gdouble inset = 0;
  if (!spotifygtk_track_list_duration_inset (self->list, &inset))
    return G_SOURCE_CONTINUE;   /* no row laid out yet */

  gint page_w = gtk_widget_get_width (GTK_WIDGET (self));

  /*
   * Line up the label, not the widget.
   *
   * A button's text sits inside its padding, so putting the button's *edge* on
   * the column left the word short of the numbers by that padding -- edges
   * aligned, text visibly not. Rather than model the padding (which read as
   * zero when asked directly), this measures where the label actually landed
   * and corrects by the difference. One step: moving the margin moves the
   * label by the same amount, and nothing here feeds back into the durations.
   */
  GtkWidget *ch = gtk_button_get_child (GTK_BUTTON (self->action_btn));
  graphene_rect_t lb;
  gint margin = gtk_widget_get_margin_end (self->title_row);

  if (!gtk_widget_get_mapped (self->action_btn) || !ch ||
      !gtk_widget_compute_bounds (ch, GTK_WIDGET (self), &lb)) {
    /* Nothing to align against yet -- put the edge roughly right and wait. */
    if (margin != (gint) inset)
      gtk_widget_set_margin_end (self->title_row, (gint) inset);
    return G_SOURCE_CONTINUE;
  }

  gdouble label_inset = page_w - (lb.origin.x + lb.size.width);
  gdouble error = label_inset - inset;

  if (ABS (error) > 1.0) {
    gint want = margin - (gint) error;
    if (want < 0 || want > page_w / 4)
      return G_SOURCE_CONTINUE;
    gtk_widget_set_margin_end (self->title_row, want);
    return G_SOURCE_CONTINUE;   /* settle, then confirm */
  }

  self->align_tick = 0;

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
  if (!visible && self->align_tick) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->align_tick);
    self->align_tick = 0;
  }
  if (visible && !self->loading && self->align_tick == 0 &&
      spotifygtk_settings_get_compact_mode (spotifygtk_settings_get_default ())) {
    self->align_attempts = 0;
    self->align_tick = gtk_widget_add_tick_callback (
      GTK_WIDGET (self), align_action_to_durations, self, NULL);
  }
  gtk_button_set_label (GTK_BUTTON (self->expanded_action_btn), label ? label : "");
  gtk_widget_set_visible (self->expanded_action_btn, visible);

  /* Colour on hover only, and only for the one that destroys something. */
  if (destructive)
    gtk_widget_add_css_class (self->action_btn, "destructive-hover");
  else
    gtk_widget_remove_css_class (self->action_btn, "destructive-hover");
  if (destructive)
    gtk_widget_add_css_class (self->expanded_action_btn, "destructive-hover");
  else
    gtk_widget_remove_css_class (self->expanded_action_btn, "destructive-hover");
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
}

void
spotifygtk_context_page_clear_spotify_cache (SpotifyGtkContextPage *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));
  g_hash_table_remove_all (self->cached_contexts);
  g_queue_clear_full (self->cache_order, g_free);
  if (self->current_uri && g_str_has_prefix (self->current_uri, "spotify:")) {
    spotifygtk_track_list_clear (self->list);
    spotifygtk_track_list_set_status (self->list,
      "Spotify cached tracks were cleared. Sign in to load this page again.");
    g_clear_pointer (&self->current_uri, g_free);
  }
}

void
spotifygtk_context_page_load (SpotifyGtkContextPage *self,
                              const gchar           *uri,
                              const gchar           *title,
                              const gchar           *kind,
                              const gchar           *cover_id,
                              GdkTexture            *preview)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));

  g_free (self->current_kind);
  self->current_kind = g_strdup (kind);

  if (!uri || !*uri)
    return;

  gboolean same_context = g_strcmp0 (uri, self->current_uri) == 0;
  /* GtkListView keeps an anchor in its model. If the header changes size
   * while rows from the previous context are still attached, GTK adjusts the
   * scroll position to keep that old row in view. Detach it before changing
   * any header label or cover; the new model is installed after the header is
   * laid out. This avoids the apparent jump to the middle of a new album. */
  if (!same_context)
    spotifygtk_track_list_clear (self->list);

  gtk_label_set_text (self->kind_label, kind ? kind : "");
  gtk_label_set_text (self->title_label, title ? title : "");
  gtk_label_set_text (self->expanded_kind, kind ? kind : "");
  gtk_label_set_text (self->expanded_title, title ? title : "");
  if (g_str_has_prefix (uri, "spotify:playlist:") &&
      (!title || !*title || g_str_has_prefix (title, "spotify:"))) {
    g_autofree gchar *cached_name = NULL, *cover = NULL;
    spotifygtk_catalog_card_get (uri, &cached_name, &cover);
    const gchar *display = cached_name ? cached_name : "Playlist";
    gtk_label_set_text (self->title_label, display);
    gtk_label_set_text (self->expanded_title, display);
  }
  /* Already showing this exactly — don't re-fetch on a repeat navigation. */
  if (same_context && !self->in_flight &&
      !g_str_has_prefix (uri, "local:")) {
    if (cover_id && g_strcmp0 (cover_id, self->context_cover_id) != 0) {
      g_free (self->context_cover_id);
      self->context_cover_id = g_strdup (cover_id);
      /* A repeated navigation from a small shelf card must not downgrade
       * the album's already-resolved full-size metadata artwork. */
      if (!self->hero_cover_id || !g_str_has_prefix (uri, "spotify:album:")) {
        g_free (self->hero_cover_id);
        self->hero_cover_id = g_strdup (cover_id);
        refresh_hero_cover (self);
      }
    }
    show_hero_preview (self, preview);
    return;
  }

  g_free (self->context_cover_id);
  self->context_cover_id = g_strdup (cover_id);
  g_clear_pointer (&self->hero_cover_id, g_free);
  self->hero_cover_id = g_strdup (cover_id);
  gtk_label_set_text (self->year_label, "");
  gtk_label_set_text (self->expanded_meta, "");
  refresh_hero_cover (self);
  show_hero_preview (self, preview);

  if (self->in_flight) {
    self->generation++;
    g_cancellable_cancel (self->in_flight);
    g_clear_object (&self->in_flight);
  }

  if (self->local_snapshot) {
    spotifygtk_track_list_clear (self->list);
    spotifygtk_local_snapshot_unref (self->local_snapshot);
    self->local_snapshot = NULL;
  }

  g_free (self->current_uri);
  self->current_uri = g_strdup (uri);

  if (g_str_has_prefix (uri, "local:album:")) {
    SpotifyGtkLocalSnapshot *snapshot = spotifygtk_local_catalog_ref_snapshot ();
    const SpotifyGtkLocalAlbum *album =
      spotifygtk_local_snapshot_find_album (snapshot, uri);
    if (!album) {
      spotifygtk_local_snapshot_unref (snapshot);
      spotifygtk_track_list_clear (self->list);
      spotifygtk_track_list_set_status (self->list,
                                        "This local album is no longer available.");
      set_loading (self, FALSE);
      return;
    }
    self->local_snapshot = snapshot;
    g_autoptr(GPtrArray) tracks = g_ptr_array_new ();
    for (guint i = 0; i < album->tracks->len; i++) {
      const SpotifyGtkLocalTrack *track = g_ptr_array_index (album->tracks, i);
      g_ptr_array_add (tracks, track->display);
    }
    update_expanded_metadata (self, tracks);
    spotifygtk_track_list_set_borrowed_native_tracks (self->list, tracks);
    spotifygtk_track_list_scroll_to_top (self->list);
    if (tracks->len == 0)
      spotifygtk_track_list_set_status (self->list, "No playable local tracks.");
    set_loading (self, FALSE);
    return;
  }

  if (g_str_has_prefix (uri, "local:playlist:")) {
    g_autoptr(GPtrArray) tracks = spotifygtk_device_playlists_tracks (
      self->device_playlists, uri, NULL);
    update_expanded_metadata (self, tracks);
    spotifygtk_track_list_set_native_tracks (self->list, tracks);
    spotifygtk_track_list_scroll_to_top (self->list);
    spotifygtk_track_list_set_status (self->list,
      tracks->len ? NULL : "No tracks in this device playlist yet.");
    set_loading (self, FALSE);
    return;
  }

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    GPtrArray *cached = g_hash_table_lookup (self->cached_contexts, uri);
    g_autoptr(GPtrArray) disk_tracks = NULL;
    if (!cached && g_str_has_prefix (uri, "spotify:playlist:")) {
      g_autofree gchar *key = spotifygtk_catalog_context_key (uri, CONTEXT_PAGE_LIMIT);
      g_autoptr(GBytes) bytes = spotifygtk_catalog_cache_get (key, 0);
      disk_tracks = spotifygtk_catalog_tracks_unpack (bytes);
      cached = disk_tracks;
    }
    gboolean playlist = g_str_has_prefix (uri, "spotify:playlist:");
    g_autoptr(GPtrArray) visible = playlist
      ? spotifygtk_device_playlists_tracks (self->device_playlists, uri, cached)
      : cached ? g_ptr_array_ref (cached) : NULL;
    if (visible && (cached || visible->len > 0)) {
      update_expanded_metadata (self, visible);
      spotifygtk_track_list_set_native_tracks (self->list, visible);
      spotifygtk_track_list_set_status (self->list, NULL);
      spotifygtk_track_list_scroll_to_top (self->list);
      set_loading (self, FALSE);
      return;
    }
    spotifygtk_track_list_clear (self->list);
    spotifygtk_track_list_set_status (self->list,
      "Sign in to open this album or playlist. Previously loaded tracks remain available.");
    g_clear_pointer (&self->current_uri, g_free);
    set_loading (self, FALSE);
    return;
  }

  spotifygtk_track_list_clear (self->list);
  spotifygtk_track_list_scroll_to_top (self->list);
  spotifygtk_track_list_set_status (self->list, NULL);
  set_loading (self, TRUE);

  self->in_flight = g_cancellable_new ();

  ContextLoad *cl = g_new0 (ContextLoad, 1);
  g_weak_ref_init (&cl->page, self);
  cl->generation = self->generation;

  spotifygtk_native_session_load_tracks (self->session, uri, CONTEXT_PAGE_LIMIT,
                                         self->in_flight, on_tracks_loaded, cl);
}

void
spotifygtk_context_page_set_device_playlists (
  SpotifyGtkContextPage *self, SpotifyGtkDevicePlaylists *playlists)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONTEXT_PAGE (self));
  self->device_playlists = playlists;
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

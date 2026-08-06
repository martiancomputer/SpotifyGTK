/*
 * track_list.c — Virtualised scrolling list of tracks.
 *
 * GtkListView, not GtkListBox: the old list built one widget per row, so a
 * few-hundred-track listing meant hundreds of live widgets each fetching a
 * cover, which grew memory without bound and eventually crashed. GtkListView
 * keeps a small pool of row widgets — roughly the visible count plus a
 * buffer — and recycles them as the list scrolls, so a list of any length
 * costs the same handful of widgets and covers.
 *
 * The model is a GListStore of SpotifyGtkTrackItem. Marking a different row
 * as playing updates that item, which signals only its bound row, rather
 * than rebuilding anything.
 *
 * The public API is unchanged from the GtkListBox version, so the pages did
 * not need to change.
 */

#include "track_list.h"
#include "context_menu.h"
#include "track_row.h"
#include "cover_loader.h"
#include "track_item.h"
#include "smooth_scroll.h"

struct _SpotifyGtkTrackList {
  GtkBox parent_instance;

  GtkLabel    *status;
  GtkListView *list;
  GListStore  *store;

  gboolean numbered;
  gboolean show_like;   /* hearts hidden on the Liked Songs page */

  /*
   * URIs known to be liked -- BORROWED from the window, not owned.
   *
   * Each list used to keep its own copy, filled by fanning every liked URI out
   * to every list as the collection was read. On a 4806-track library that is
   * five hash tables holding five copies of every string, built by a loop over
   * (URIs x lists x bound rows). One shared table costs one copy and no fan-out.
   */
  GHashTable *liked_set;

  /* Scroll settle detection. Cover loading is suppressed while the adjustment
   * is moving and resumed once it has been still for SETTLE_MS. */
  GtkAdjustment *vadj;         /* borrowed */
  guint          settle_id;
  GPtrArray     *bound_rows;   /* borrowed SpotifyGtkTrackRow*, live bindings */
};

/*
 * Long enough that an eased wheel scroll is treated as one gesture rather than
 * a run of separate ones, short enough that art appears promptly after
 * stopping. The animation itself runs ~150ms, so anything below that would
 * fire mid-scroll and defeat the purpose.
 */
#define SETTLE_MS 180

/* Rows warmed either side of the visible range once the scroll settles, in the
 * direction of travel. Bounded deliberately: this is meant to make the next
 * flick start warm, not to fetch the whole collection. */
#define PREFETCH_ROWS 24

G_DEFINE_FINAL_TYPE (SpotifyGtkTrackList, spotifygtk_track_list, GTK_TYPE_BOX)

static gboolean
on_scroll_settled (gpointer user_data)
{
  SpotifyGtkTrackList *self = user_data;
  self->settle_id = 0;

  /* Loading first, prefetch second: what is on screen must not queue behind
   * speculative work. */
  spotifygtk_cover_set_deferred (FALSE);

  for (guint i = 0; i < self->bound_rows->len; i++)
    spotifygtk_track_row_retry_cover (g_ptr_array_index (self->bound_rows, i));

  /* Warm ahead in the direction just travelled. The visible window is derived
   * from the adjustment rather than asked of the view, which has no API for
   * it; approximate is fine for a prefetch. */
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  if (n > 0 && self->vadj) {
    gdouble upper = gtk_adjustment_get_upper (self->vadj);
    gdouble page  = gtk_adjustment_get_page_size (self->vadj);
    gdouble value = gtk_adjustment_get_value (self->vadj);
    if (upper > page) {
      gdouble frac = value / (upper - page);
      guint   last = (guint) (frac * (gdouble) n) + (guint) (page / 56.0);
      for (guint i = last; i < MIN (last + PREFETCH_ROWS, n); i++) {
        g_autoptr(SpotifyGtkTrackItem) item =
          g_list_model_get_item (G_LIST_MODEL (self->store), i);
        const SpotifyNativeTrack *t = item ? spotifygtk_track_item_get_track (item) : NULL;
        if (t && t->cover_id)
          spotifygtk_cover_prefetch (t->cover_id, 96);
      }
    }
  }

  /* Reported here because settling is the natural boundary: everything a
   * gesture asked for has resolved by now. Behind G_MESSAGES_DEBUG so it costs
   * nothing in a normal run. */
  if (g_getenv ("SPOTIFY_COVER_STATS"))
    spotifygtk_cover_log_stats ("scroll settled");

  return G_SOURCE_REMOVE;
}

static void
on_vadj_changed (GtkAdjustment *adj, gpointer user_data)
{
  SpotifyGtkTrackList *self = user_data;
  (void) adj;

  /* Any movement re-arms the timer, so a continuous scroll never settles until
   * it actually stops. */
  spotifygtk_cover_set_deferred (TRUE);
  if (self->settle_id)
    g_source_remove (self->settle_id);
  self->settle_id = g_timeout_add (SETTLE_MS, on_scroll_settled, self);
}

enum { TRACK_ACTIVATED, ADD_TO_QUEUE, GO_TO_ALBUM, GO_TO_ARTIST,
       ADD_TO_LIKED, REMOVE_FROM_LIKED, ADD_TO_PLAYLIST, N_SIGNALS };
static guint signals[N_SIGNALS];

/* === Right-click context menu === */

/* One menu's worth of state: the list it belongs to and an owned copy of the
 * clicked track, so the menu survives the row being recycled while it is open.
 * Freed when the popover is destroyed. */
typedef struct {
  SpotifyGtkTrackList *list;
  SpotifyNativeTrack  *track;   /* owned copy */
} MenuCtx;

static void
menu_ctx_free (gpointer data)
{
  MenuCtx *ctx = data;
  spotifygtk_native_track_free (ctx->track);
  g_free (ctx);
}

static void
menu_emit_and_close (GtkButton *button, guint signal_id)
{
  GtkWidget *w = GTK_WIDGET (button);
  MenuCtx *ctx = spotifygtk_context_menu_get_context (w);
  if (ctx)
    g_signal_emit (ctx->list, signals[signal_id], 0, (gpointer) ctx->track);

  GtkPopover *popover = spotifygtk_context_menu_get_popover (w);
  if (popover)
    gtk_popover_popdown (popover);
}

static void on_menu_add_to_liked      (GtkButton *b, gpointer d) { (void) d; menu_emit_and_close (b, ADD_TO_LIKED); }
static void on_menu_remove_from_liked (GtkButton *b, gpointer d) { (void) d; menu_emit_and_close (b, REMOVE_FROM_LIKED); }
static void on_menu_add_to_playlist (GtkButton *b, gpointer d) { (void) d; menu_emit_and_close (b, ADD_TO_PLAYLIST); }
static void on_menu_add_to_queue (GtkButton *b, gpointer d) { (void) d; menu_emit_and_close (b, ADD_TO_QUEUE); }
static void on_menu_go_to_album  (GtkButton *b, gpointer d) { (void) d; menu_emit_and_close (b, GO_TO_ALBUM); }
static void on_menu_go_to_artist (GtkButton *b, gpointer d) { (void) d; menu_emit_and_close (b, GO_TO_ARTIST); }

static void
on_row_secondary_pressed (GtkGestureClick *gesture, gint n_press,
                          gdouble x, gdouble y, gpointer user_data)
{
  SpotifyGtkTrackList *self = user_data;
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

  SpotifyGtkTrackItem *item = g_object_get_data (G_OBJECT (row), "bound-item");
  if (!item)
    return;

  const SpotifyNativeTrack *track = spotifygtk_track_item_get_track (item);

  MenuCtx *ctx = g_new0 (MenuCtx, 1);
  ctx->list  = self;
  ctx->track = spotifygtk_native_track_copy (track);

  SpotifyGtkContextMenu *menu = spotifygtk_context_menu_new ();

  /*
   * One entry that names what it will do, rather than a shape the reader has
   * to interpret. This is why the row indicator can be passive: the menu says
   * "Remove" when the track is already saved.
   */
  gboolean liked = spotifygtk_track_row_get_liked (SPOTIFYGTK_TRACK_ROW (row));
  spotifygtk_context_menu_add (menu,
                               liked ? "Remove from Liked Songs"
                                     : "Add to Liked Songs",
                               TRUE, NULL,
                               liked ? G_CALLBACK (on_menu_remove_from_liked)
                                     : G_CALLBACK (on_menu_add_to_liked), NULL);
  spotifygtk_context_menu_add (menu, "Add to Playlist…", TRUE, NULL,
                               G_CALLBACK (on_menu_add_to_playlist), NULL);
  spotifygtk_context_menu_add (menu, "Add to Queue", TRUE, NULL,
                               G_CALLBACK (on_menu_add_to_queue), NULL);
  spotifygtk_context_menu_add (menu, "Go to Artist",
                               ctx->track->artist_uri != NULL, NULL,
                               G_CALLBACK (on_menu_go_to_artist), NULL);
  spotifygtk_context_menu_add (menu, "Go to Album",
                               ctx->track->album_uri != NULL, NULL,
                               G_CALLBACK (on_menu_go_to_album), NULL);
  spotifygtk_context_menu_add_separator (menu);
  spotifygtk_context_menu_add (menu, "Share track", FALSE,
                               "Not implemented yet", NULL, NULL);

  spotifygtk_context_menu_present (menu, row, x, y, ctx, menu_ctx_free);

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  (void) n_press;
}

/* === Factory === */

static void
on_list_item_position (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  GtkListItem         *list_item = GTK_LIST_ITEM (object);
  SpotifyGtkTrackList *self      = user_data;
  GtkWidget           *child     = gtk_list_item_get_child (list_item);
  (void) pspec;

  if (self->numbered && SPOTIFYGTK_IS_TRACK_ROW (child))
    spotifygtk_track_row_set_number (SPOTIFYGTK_TRACK_ROW (child),
                                     (gint) gtk_list_item_get_position (list_item) + 1);
}

static void
factory_setup (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  GtkWidget *row = GTK_WIDGET (spotifygtk_track_row_new ());

  /* Right-click anywhere on the row opens the context menu. Added once per
   * pooled row widget; the handler reads whichever item is bound at click
   * time, so it follows the row through recycling. */
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_SECONDARY);
  g_signal_connect (click, "pressed", G_CALLBACK (on_row_secondary_pressed), user_data);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (click));

  /*
   * A row whose position shifts under a splice is not re-bound by GTK -- the
   * same item is still there, just further up -- so the number has to follow
   * the property. Connected here rather than in bind: the list item is pooled,
   * so a per-bind connect would stack up a handler per recycle.
   */
  g_signal_connect (list_item, "notify::position",
                    G_CALLBACK (on_list_item_position), user_data);

  gtk_list_item_set_child (list_item, row);
  (void) factory;
}

/* Item -> row, called each time a recycled row is pointed at a new track. */
static void
on_item_changed (SpotifyGtkTrackItem *item, gpointer user_data)
{
  SpotifyGtkTrackRow *row = user_data;
  spotifygtk_track_row_set_playing (row,
                                    spotifygtk_track_item_get_playing (item),
                                    spotifygtk_track_item_get_paused (item));
}

static void
on_row_play_clicked (SpotifyGtkTrackRow *row, gpointer user_data)
{
  SpotifyGtkTrackList *self = user_data;
  SpotifyGtkTrackItem *item = g_object_get_data (G_OBJECT (row), "bound-item");
  if (item)
    g_signal_emit (self, signals[TRACK_ACTIVATED], 0,
                   (gpointer) spotifygtk_track_item_get_track (item));
}


static void
factory_bind (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  SpotifyGtkTrackList *self = user_data;
  SpotifyGtkTrackRow  *row  = SPOTIFYGTK_TRACK_ROW (gtk_list_item_get_child (list_item));
  SpotifyGtkTrackItem *item = gtk_list_item_get_item (list_item);

  if (!g_ptr_array_find (self->bound_rows, row, NULL))
    g_ptr_array_add (self->bound_rows, row);

  /*
   * Number from the position in the model, not from the item.
   *
   * The item's number is fixed when the list is built, so it only stays right
   * as long as nothing is ever inserted or removed. Deriving it here means a
   * single-row splice renumbers correctly, which is what lets an unlike remove
   * one row instead of rebuilding the whole list.
   */
  spotifygtk_track_row_set_native_track (row,
    spotifygtk_track_item_get_track (item), 0);
  spotifygtk_track_row_set_number (row,
    self->numbered ? (gint) gtk_list_item_get_position (list_item) + 1 : 0);
  spotifygtk_track_row_set_playing (row,
    spotifygtk_track_item_get_playing (item),
    spotifygtk_track_item_get_paused (item));
  spotifygtk_track_row_set_like_visible (row, self->show_like);

  const SpotifyNativeTrack *bt = spotifygtk_track_item_get_track (item);
  spotifygtk_track_row_set_liked (row,
    self->liked_set && bt && bt->uri &&
    g_hash_table_contains (self->liked_set, bt->uri));

  /* Connect for the lifetime of this binding; disconnected in unbind. The
   * item ref is stashed so play-clicked knows which track it is. */
  g_object_set_data_full (G_OBJECT (row), "bound-item",
                          g_object_ref (item), g_object_unref);

  gulong changed = g_signal_connect (item, "changed",
                                     G_CALLBACK (on_item_changed), row);
  g_object_set_data (G_OBJECT (row), "changed-handler", GSIZE_TO_POINTER (changed));

  if (!g_object_get_data (G_OBJECT (row), "play-connected")) {
    g_signal_connect (row, "play-clicked", G_CALLBACK (on_row_play_clicked), self);
    g_object_set_data (G_OBJECT (row), "play-connected", GINT_TO_POINTER (1));
  }

  (void) factory;
}

static void
factory_unbind (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  SpotifyGtkTrackRow  *row  = SPOTIFYGTK_TRACK_ROW (gtk_list_item_get_child (list_item));
  SpotifyGtkTrackItem *item = gtk_list_item_get_item (list_item);

  gulong changed = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (row), "changed-handler"));
  if (changed && item)
    g_signal_handler_disconnect (item, changed);
  g_object_set_data (G_OBJECT (row), "changed-handler", NULL);
  g_object_set_data (G_OBJECT (row), "bound-item", NULL);
  g_ptr_array_remove_fast (SPOTIFYGTK_TRACK_LIST (user_data)->bound_rows, row);

  (void) factory;
}

/* === Activation === */

static void
on_list_activate (GtkListView *list, guint position, gpointer user_data)
{
  SpotifyGtkTrackList *self = user_data;
  g_autoptr(SpotifyGtkTrackItem) item =
    g_list_model_get_item (G_LIST_MODEL (self->store), position);
  if (item)
    g_signal_emit (self, signals[TRACK_ACTIVATED], 0,
                   (gpointer) spotifygtk_track_item_get_track (item));
  (void) list;
}

/* === Boilerplate === */

static void
spotifygtk_track_list_dispose (GObject *object)
{
  SpotifyGtkTrackList *self = SPOTIFYGTK_TRACK_LIST (object);

  /* The settle timer holds a plain pointer to this list, so it must not outlive
   * it. Clearing deferral matters just as much: left switched on, the next page
   * built would treat every cache miss as skippable and never show any art. */
  if (self->settle_id) {
    g_source_remove (self->settle_id);
    self->settle_id = 0;
  }
  spotifygtk_cover_set_deferred (FALSE);
  g_clear_pointer (&self->bound_rows, g_ptr_array_unref);

  g_clear_object (&self->store);
  G_OBJECT_CLASS (spotifygtk_track_list_parent_class)->dispose (object);
}

static void
spotifygtk_track_list_class_init (SpotifyGtkTrackListClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_track_list_dispose;

  signals[TRACK_ACTIVATED] = g_signal_new ("track-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);

  signals[ADD_TO_QUEUE] = g_signal_new ("add-to-queue",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);

  signals[GO_TO_ALBUM] = g_signal_new ("go-to-album",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);

  signals[GO_TO_ARTIST] = g_signal_new ("go-to-artist",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);

  /* Emitted with the track; nothing is connected yet -- the collection write
   * it needs is not implemented. The menu entry is live so the plumbing can be
   * exercised, but liking a track currently does nothing. */
  signals[ADD_TO_LIKED] = g_signal_new ("add-to-liked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);

  signals[REMOVE_FROM_LIKED] = g_signal_new ("remove-from-liked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);

  signals[ADD_TO_PLAYLIST] = g_signal_new ("add-to-playlist",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
spotifygtk_track_list_init (SpotifyGtkTrackList *self)
{
  self->show_like = TRUE;
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 8);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  self->status = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->status), "dim-text");
  gtk_label_set_xalign (self->status, 0.0);
  gtk_widget_set_visible (GTK_WIDGET (self->status), FALSE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->status));

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (scroller),
                                   GTK_ORIENTATION_VERTICAL);

  self->vadj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));
  if (self->vadj)
    g_signal_connect (self->vadj, "value-changed", G_CALLBACK (on_vadj_changed), self);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);

  self->store = g_list_store_new (SPOTIFYGTK_TYPE_TRACK_ITEM);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup",  G_CALLBACK (factory_setup),  self);
  self->bound_rows = g_ptr_array_new ();
  g_signal_connect (factory, "bind",   G_CALLBACK (factory_bind),   self);
  g_signal_connect (factory, "unbind", G_CALLBACK (factory_unbind), self);

  /* No-selection: the list is a play surface, not a picker, so there is no
   * persistent selection highlight. Single-click activation makes one click
   * on a row play it, matching the old GtkListBox behaviour. */
  GtkNoSelection *model = gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->store)));
  self->list = GTK_LIST_VIEW (gtk_list_view_new (GTK_SELECTION_MODEL (model), factory));
  gtk_list_view_set_single_click_activate (self->list, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "track-listview");
  gtk_widget_set_margin_end (GTK_WIDGET (self->list), 2);
  g_signal_connect (self->list, "activate", G_CALLBACK (on_list_activate), self);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (self->list));
  gtk_box_append (GTK_BOX (self), scroller);
}

void
spotifygtk_track_list_set_top_inset (SpotifyGtkTrackList *self, gint px)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  /* On the listview so the gap is part of the scrollable content and scrolls
   * under the header; on the status label too, so an empty/loading message
   * clears the header rather than hiding behind it. */
  gtk_widget_set_margin_top (GTK_WIDGET (self->list), px);
  gtk_widget_set_margin_top (GTK_WIDGET (self->status), px);
}

/*
 * Hearts are hidden on the Liked Songs page: every row there is liked by
 * definition, so a column of identical filled hearts carries no information
 * and only invites a misclick. Everywhere else -- albums, playlists, search --
 * the state varies and the control earns its place.
 */
void
spotifygtk_track_list_set_show_like (SpotifyGtkTrackList *self, gboolean show)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));
  self->show_like = show;

  for (guint i = 0; self->bound_rows && i < self->bound_rows->len; i++)
    spotifygtk_track_row_set_like_visible (
      SPOTIFYGTK_TRACK_ROW (g_ptr_array_index (self->bound_rows, i)), show);
}

/*
 * Borrow the window's set of liked URIs. Consulted whenever a row binds, so
 * scrolling shows the right hearts without this list holding any state of its
 * own.
 */
void
spotifygtk_track_list_set_liked_set (SpotifyGtkTrackList *self, GHashTable *set)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));
  self->liked_set = set;
}

/*
 * Repaint the hearts of rows currently on screen.
 *
 * For after the shared set changes. Touches only live bindings -- a handful of
 * rows -- rather than walking the collection, which is what made loading a
 * large library crawl.
 */
void
spotifygtk_track_list_refresh_liked (SpotifyGtkTrackList *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));
  if (!self->liked_set || !self->bound_rows)
    return;

  for (guint i = 0; i < self->bound_rows->len; i++) {
    GtkWidget *row = g_ptr_array_index (self->bound_rows, i);
    SpotifyGtkTrackItem *item = g_object_get_data (G_OBJECT (row), "bound-item");
    const SpotifyNativeTrack *t = item ? spotifygtk_track_item_get_track (item) : NULL;
    if (t && t->uri)
      spotifygtk_track_row_set_liked (SPOTIFYGTK_TRACK_ROW (row),
                                      g_hash_table_contains (self->liked_set, t->uri));
  }
}

/*
 * Drop every row for `uri` from the model.
 *
 * For the Liked Songs page after an unlike. Refetching instead would mean
 * pulling the whole collection again -- thousands of tracks and their art --
 * to reflect one removal, and would race the server besides: the write and the
 * read are different services, and a read issued straight after a write can
 * still return the old set.
 */

/*
 * Single-row splices, for a list whose membership changed by exactly one.
 *
 * The alternative is handing set_native_tracks() the new full listing, which
 * rebuilds every row and drops the scroll position -- on a 4800-track library
 * that throws the user from wherever they were back to the top. Row numbers
 * follow the position property, so the rows below renumber themselves.
 */
void
spotifygtk_track_list_insert_native_track (SpotifyGtkTrackList      *self,
                                           guint                     position,
                                           const SpotifyNativeTrack *track)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));
  g_return_if_fail (track != NULL);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  position = MIN (position, n);

  /* The number is set from the position on bind, so the value passed here is
   * only a placeholder. */
  g_autoptr(SpotifyGtkTrackItem) item = spotifygtk_track_item_new (track, position + 1);
  g_list_store_insert (self->store, position, item);

  spotifygtk_track_list_set_status (self, NULL);
}

void
spotifygtk_track_list_remove_position (SpotifyGtkTrackList *self, guint position)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  if (position >= g_list_model_get_n_items (G_LIST_MODEL (self->store)))
    return;
  g_list_store_remove (self->store, position);
}

SpotifyGtkTrackList *
spotifygtk_track_list_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_TRACK_LIST, NULL);
}

void
spotifygtk_track_list_clear (SpotifyGtkTrackList *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));
  g_list_store_remove_all (self->store);
}

void
spotifygtk_track_list_set_status (SpotifyGtkTrackList *self, const gchar *message)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  if (message && *message) {
    gtk_label_set_text (self->status, message);
    gtk_widget_set_visible (GTK_WIDGET (self->status), TRUE);
  } else {
    gtk_widget_set_visible (GTK_WIDGET (self->status), FALSE);
  }
}

void
spotifygtk_track_list_set_numbered (SpotifyGtkTrackList *self, gboolean numbered)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));
  self->numbered = numbered;
}

void
spotifygtk_track_list_set_native_tracks (SpotifyGtkTrackList *self, GPtrArray *tracks)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  guint existing = g_list_model_get_n_items (G_LIST_MODEL (self->store));

  if (!tracks || tracks->len == 0) {
    g_list_store_remove_all (self->store);
    spotifygtk_track_list_set_status (self, "Nothing here yet.");
    return;
  }

  /*
   * Build the whole set, then splice it in as one operation.
   *
   * g_list_store_append() in a loop emits items-changed once per item, and
   * GtkListView does real work on each -- so replacing a 4800-track listing
   * meant 4800 emissions and 4800 rounds of view revalidation. That is what
   * made clearing the Liked Songs filter stutter hardest: an empty query is
   * precisely the case that hands the list everything.
   *
   * A splice is one emission, whatever the size, and it also replaces the
   * separate remove_all that used to emit its own.
   */
  GPtrArray *items = g_ptr_array_new_full (tracks->len, g_object_unref);

  for (guint i = 0; i < tracks->len; i++) {
    const SpotifyNativeTrack *track = g_ptr_array_index (tracks, i);
    if (!track || !track->uri)
      continue;
    g_ptr_array_add (items, spotifygtk_track_item_new (track, items->len + 1));
  }

  g_list_store_splice (self->store, 0, existing, items->pdata, items->len);

  guint shown = items->len;
  g_ptr_array_unref (items);

  spotifygtk_track_list_set_status (self, shown == 0 ? "Nothing playable here." : NULL);
}

GPtrArray *
spotifygtk_track_list_snapshot (SpotifyGtkTrackList *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self), NULL);

  GPtrArray *out = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_track_free);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (guint i = 0; i < n; i++) {
    g_autoptr(SpotifyGtkTrackItem) item =
      g_list_model_get_item (G_LIST_MODEL (self->store), i);
    g_ptr_array_add (out,
                     spotifygtk_native_track_copy (spotifygtk_track_item_get_track (item)));
  }

  return out;
}

void
spotifygtk_track_list_set_playing_uri (SpotifyGtkTrackList *self,
                                       const gchar *uri,
                                       gboolean     playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (guint i = 0; i < n; i++) {
    g_autoptr(SpotifyGtkTrackItem) item =
      g_list_model_get_item (G_LIST_MODEL (self->store), i);
    const gchar *item_uri = spotifygtk_track_item_get_uri (item);
    gboolean is_current = uri && item_uri && g_strcmp0 (uri, item_uri) == 0;

    /* Only the current track animates; the rest are cleared. A paused
     * current track keeps its indicator but stops moving. */
    spotifygtk_track_item_set_playing (item, is_current, is_current && !playing);
  }
}

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
#include "track_row.h"
#include "track_item.h"

struct _SpotifyGtkTrackList {
  GtkBox parent_instance;

  GtkLabel    *status;
  GtkListView *list;
  GListStore  *store;

  gboolean numbered;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkTrackList, spotifygtk_track_list, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* === Factory === */

static void
factory_setup (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  gtk_list_item_set_child (list_item, GTK_WIDGET (spotifygtk_track_row_new ()));
  (void) factory; (void) user_data;
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

  spotifygtk_track_row_set_native_track (row,
    spotifygtk_track_item_get_track (item),
    self->numbered ? (gint) (spotifygtk_track_item_get_number (item)) : 0);
  spotifygtk_track_row_set_playing (row,
    spotifygtk_track_item_get_playing (item),
    spotifygtk_track_item_get_paused (item));

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

  (void) factory; (void) user_data;
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
}

static void
spotifygtk_track_list_init (SpotifyGtkTrackList *self)
{
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
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);

  self->store = g_list_store_new (SPOTIFYGTK_TYPE_TRACK_ITEM);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup",  G_CALLBACK (factory_setup),  self);
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

  g_list_store_remove_all (self->store);

  if (!tracks || tracks->len == 0) {
    spotifygtk_track_list_set_status (self, "Nothing here yet.");
    return;
  }

  guint shown = 0;
  for (guint i = 0; i < tracks->len; i++) {
    const SpotifyNativeTrack *track = g_ptr_array_index (tracks, i);
    if (!track || !track->uri)
      continue;

    g_autoptr(SpotifyGtkTrackItem) item = spotifygtk_track_item_new (track, shown + 1);
    g_list_store_append (self->store, item);
    shown++;
  }

  spotifygtk_track_list_set_status (self, shown == 0 ? "Nothing playable here." : NULL);
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

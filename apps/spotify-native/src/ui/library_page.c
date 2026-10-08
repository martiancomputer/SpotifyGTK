/*
 * library_page.c — Library page implementation.
 *
 * What is real here: the albums grid is the distinct albums present in the
 * signed-in user's Liked Songs, resolved through the same /context-resolve
 * path everything else uses. Those are genuinely albums the user has saved
 * tracks from, so the grid is real library data, not filler.
 *
 * The optional unified view reuses the same virtualized card grid for saved
 * releases, local albums, and playlist cards. No nested scroller is added.
 */

#include "library_page.h"
#include "album_grid.h"
#include "local_catalog.h"
#include "settings.h"
#include "../log_file.h"
#include "../spotify/collection.h"
#include "../spotify/catalog_cache.h"

/* Artist identities share the bounded catalogue cache with the session.
 * Do not rewrite a whole name map synchronously on GTK's thread per card. */

typedef enum {
  LIBRARY_ALL = 0,
  LIBRARY_ALBUMS,
  LIBRARY_EPS,
  LIBRARY_SINGLES,
  LIBRARY_ARTISTS,
  LIBRARY_LOCAL,
  LIBRARY_PLAYLISTS,
  N_LIBRARY_VIEWS
} LibraryView;

struct _SpotifyGtkLibraryPage {
  GtkBox parent_instance;

  SpotifyGtkAlbumGrid  *albums;
  SpotifyGtkAlbumGrid  *albums_primary;
  SpotifyGtkAlbumGrid  *albums_alt;
  SpotifyGtkAlbumGrid  *artists;
  GtkWidget            *albums_status;   /* empty/error note; loading uses window overlay */
  GtkWidget            *albums_status_primary;
  GtkWidget            *albums_status_alt;
  GtkWidget            *local_empty_primary;
  GtkWidget            *local_empty_alt;
  GtkWidget            *artists_status;
  GtkStack             *content_stack;
  GtkSearchEntry       *filter_entry;
  GtkWidget            *view_buttons[N_LIBRARY_VIEWS];
  LibraryView           active_view;
  GPtrArray            *saved_releases; /* SpotifyNativeRelease*, owned */
  GPtrArray            *playlists; /* owned playlist card snapshots */
  gboolean              separate_playlists;
  SpotifyGtkLocalSnapshot *local_snapshot; /* referenced; immutable until swap */
  GPtrArray            *saved_album_uris; /* gchar*, gathered across pages */
  GHashTable           *saved_album_dates; /* album URI -> added_at */
  GPtrArray            *followed_artist_uris; /* gchar*, copied from window */
  gboolean              artists_populated;
  /* Invalidates callbacks from a previous load. Without it a second load
   * clears and re-splices the model while the first one's reads are still
   * arriving, and a splice destroys the item a bound card is showing -- the
   * failure album_grid.c's add_card comment describes. */
  guint                 generation;
  GtkWidget            *header_revealer; /* title + heading, folds away on scroll */
  GtkWidget            *new_playlist_button;
  SpotifyNativeSession *session;         /* not owned */
  GCancellable         *load_cancel;
  guint                 load_timeout_id;
  guint                 load_retry_count;
  gboolean              loading;
  gboolean              loaded;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkLibraryPage, spotifygtk_library_page, GTK_TYPE_BOX)

enum { LOADING_CHANGED, NEW_PLAYLIST_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
set_loading (SpotifyGtkLibraryPage *self, gboolean loading)
{
  loading = !!loading;
  if (self->loading == loading)
    return;
  self->loading = loading;
  g_signal_emit (self, signals[LOADING_CHANGED], 0, loading);
}

static void
spotifygtk_library_page_dispose (GObject *object)
{
  SpotifyGtkLibraryPage *self = SPOTIFYGTK_LIBRARY_PAGE (object);
  if (self->load_cancel) {
    g_cancellable_cancel (self->load_cancel);
    g_clear_object (&self->load_cancel);
  }
  g_clear_handle_id (&self->load_timeout_id, g_source_remove);
  g_clear_pointer (&self->saved_album_uris, g_ptr_array_unref);
  g_clear_pointer (&self->saved_album_dates, g_hash_table_unref);
  g_clear_pointer (&self->saved_releases, g_ptr_array_unref);
  g_clear_pointer (&self->playlists, g_ptr_array_unref);
  spotifygtk_local_snapshot_unref (self->local_snapshot);
  self->local_snapshot = NULL;
  g_clear_pointer (&self->followed_artist_uris, g_ptr_array_unref);
  self->session = NULL;
  G_OBJECT_CLASS (spotifygtk_library_page_parent_class)->dispose (object);
}

static void
spotifygtk_library_page_class_init (SpotifyGtkLibraryPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_library_page_dispose;
  signals[LOADING_CHANGED] = g_signal_new ("loading-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[NEW_PLAYLIST_REQUESTED] = g_signal_new ("new-playlist-requested",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void
on_new_playlist_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;
  g_signal_emit (self, signals[NEW_PLAYLIST_REQUESTED], 0);
  (void) button;
}

/* === Loading saved albums === */

/*
 * The albums the user actually saved, not the albums their liked songs happen
 * to sit on.
 *
 * This grid used to be built by grouping liked tracks, which was the only
 * thing available before saved albums could be read: liking one song put its
 * whole album here, and nothing the user did to this page could remove it.
 * Saved albums live in the collection set alongside liked tracks, told apart
 * by the URI -- see research/library-writes.md -- so the real list is a filter
 * over that read, and the names and covers are one batched lookup.
 */
typedef struct { GWeakRef page; guint generation; } LibLoad;
static void start_library_load (SpotifyGtkLibraryPage *self);

static gboolean
on_library_timeout (gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;
  self->load_timeout_id = 0;
  if (!self->load_cancel)
    return G_SOURCE_REMOVE;

  /* Mercury reads cannot be cancelled, so the generation guards their late
   * replies. The metadata phase does obey the cancellable. */
  self->generation++;
  g_cancellable_cancel (self->load_cancel);
  g_clear_object (&self->load_cancel);
  set_loading (self, FALSE);
  if (self->load_retry_count++ == 0) {
    g_message ("library: read timed out; retrying once");
    start_library_load (self);
  } else {
    gtk_label_set_text (GTK_LABEL (self->albums_status),
                        "Library took too long to load. Reopen it to retry.");
    gtk_widget_set_visible (self->albums_status, TRUE);
  }
  return G_SOURCE_REMOVE;
}

static void
arm_library_timeout (SpotifyGtkLibraryPage *self)
{
  g_clear_handle_id (&self->load_timeout_id, g_source_remove);
  self->load_timeout_id = g_timeout_add_seconds (30, on_library_timeout, self);
}

static gboolean
release_in_view (const SpotifyNativeRelease *release, LibraryView view)
{
  switch (view) {
    case LIBRARY_ALL:
      return TRUE;
    case LIBRARY_LOCAL:
    case LIBRARY_PLAYLISTS:
      return FALSE;
    case LIBRARY_EPS:
      return release->type == SPOTIFY_ALBUM_TYPE_EP;
    case LIBRARY_SINGLES:
      return release->type == SPOTIFY_ALBUM_TYPE_SINGLE;
    case LIBRARY_ALBUMS:
      return release->type == SPOTIFY_ALBUM_TYPE_ALBUM ||
             release->type == SPOTIFY_ALBUM_TYPE_COMPILATION ||
             release->type == SPOTIFY_ALBUM_TYPE_UNKNOWN;
    case LIBRARY_ARTISTS:
    default:
      return FALSE;
  }
}

static gint
release_date_compare (gconstpointer a, gconstpointer b, gpointer user_data)
{
  const SpotifyNativeRelease *ra = *(SpotifyNativeRelease * const *) a;
  const SpotifyNativeRelease *rb = *(SpotifyNativeRelease * const *) b;
  GHashTable *dates = user_data;
  gint64 da = GPOINTER_TO_INT (g_hash_table_lookup (dates, ra->uri));
  gint64 db = GPOINTER_TO_INT (g_hash_table_lookup (dates, rb->uri));
  if (da != db)
    return da > db ? -1 : 1;
  return g_strcmp0 (ra->name, rb->name);
}

static void
sort_releases_by_date_added (SpotifyGtkLibraryPage *self)
{
  if (!self->saved_releases || !self->saved_album_dates)
    return;

  g_ptr_array_sort_with_data (self->saved_releases, release_date_compare,
                              self->saved_album_dates);
}

typedef struct {
  const SpotifyNativeRelease *release;
  const SpotifyGtkLocalAlbum *local;
  const SpotifyGtkCardSpec *playlist;
  gint64 date;
} LibraryCardEntry;

static void
library_playlist_free (gpointer data)
{
  SpotifyGtkCardSpec *card = data;
  g_free ((gchar *) card->uri);
  g_free ((gchar *) card->title);
  g_free ((gchar *) card->subtitle);
  g_free ((gchar *) card->cover_id);
  g_free (card);
}

static gint
library_card_compare (gconstpointer a, gconstpointer b)
{
  const LibraryCardEntry *left = a;
  const LibraryCardEntry *right = b;
  if (left->date != right->date)
    return left->date > right->date ? -1 : 1;
  const gchar *left_name = left->playlist ? left->playlist->title :
    left->local ? left->local->title : left->release->name;
  const gchar *right_name = right->playlist ? right->playlist->title :
    right->local ? right->local->title : right->release->name;
  return g_strcmp0 (left_name, right_name);
}

static void
show_release_view (SpotifyGtkLibraryPage *self)
{
  if (self->active_view == LIBRARY_ARTISTS)
    return;

  GArray *entries = g_array_new (FALSE, FALSE, sizeof (LibraryCardEntry));
  for (guint i = 0; self->saved_releases &&
                    i < self->saved_releases->len; i++) {
    const SpotifyNativeRelease *release =
      g_ptr_array_index (self->saved_releases, i);
    if (!release_in_view (release, self->active_view))
      continue;
    LibraryCardEntry entry = { .release = release,
      .date = self->saved_album_dates ? GPOINTER_TO_INT (
        g_hash_table_lookup (self->saved_album_dates, release->uri)) : 0 };
    g_array_append_val (entries, entry);
  }
  if (self->active_view == LIBRARY_ALL ||
      self->active_view == LIBRARY_LOCAL) {
    const GPtrArray *albums = spotifygtk_local_snapshot_albums (
      self->local_snapshot);
    for (guint i = 0; albums && i < albums->len; i++) {
      const SpotifyGtkLocalAlbum *album = g_ptr_array_index (
        (GPtrArray *) albums, i);
      LibraryCardEntry entry = { .local = album,
                          .date = album->added_at / G_USEC_PER_SEC };
      g_array_append_val (entries, entry);
    }
  }
  if (!self->separate_playlists &&
      (self->active_view == LIBRARY_ALL ||
       self->active_view == LIBRARY_PLAYLISTS)) {
    for (guint i = 0; self->playlists && i < self->playlists->len; i++) {
      const SpotifyGtkCardSpec *playlist = g_ptr_array_index (self->playlists, i);
      LibraryCardEntry entry = { .playlist = playlist,
                                 .date = playlist->added_at > 0
                                   ? playlist->added_at : -(gint64) i - 1 };
      g_array_append_val (entries, entry);
    }
  }

  /* Sort once before the grid's batched model replacement. */
  g_array_sort (entries, library_card_compare);

  g_autofree SpotifyGtkCardSpec *specs = g_new0 (SpotifyGtkCardSpec,
                                                 MAX (entries->len, 1));
  g_autoptr(GPtrArray) subs = g_ptr_array_new_with_free_func (g_free);
  guint out = 0;
  for (guint i = 0; i < entries->len; i++) {
    LibraryCardEntry *entry = &g_array_index (entries, LibraryCardEntry, i);
    const SpotifyGtkLocalAlbum *local = entry->local;
    const SpotifyNativeRelease *release = entry->release;
    const SpotifyGtkCardSpec *playlist = entry->playlist;
    gint year = playlist ? 0 : local ? local->year : release->year;
    gchar *sub = playlist ? g_strdup (playlist->subtitle) :
      year > 0 ? g_strdup_printf ("%d", year) : g_strdup ("");
    g_ptr_array_add (subs, sub);
    specs[out].uri      = playlist ? playlist->uri : local ? local->uri : release->uri;
    specs[out].title    = playlist ? playlist->title : local ? local->title :
                          release->name ? release->name : "Unknown release";
    specs[out].subtitle = sub;
    specs[out].cover_id = playlist ? playlist->cover_id :
                          local ? local->cover_id : release->cover_id;
    out++;
  }
  spotifygtk_album_grid_set_pending_cards (self->albums, specs, out);
  g_array_unref (entries);

  const gchar *empty = self->active_view == LIBRARY_EPS
    ? "No saved EPs yet."
    : self->active_view == LIBRARY_SINGLES
      ? "No saved singles yet."
      : self->active_view == LIBRARY_ALL
        ? "No saved or local releases yet."
      : self->active_view == LIBRARY_PLAYLISTS
        ? "No playlists yet."
      : "No saved albums yet.";
  gboolean local_empty = self->active_view == LIBRARY_LOCAL && out == 0;
  GtkWidget *empty_state = self->albums == self->albums_primary
    ? self->local_empty_primary : self->local_empty_alt;
  gtk_widget_set_visible (empty_state, local_empty);
  gtk_widget_set_visible (self->albums_status, out == 0 && !local_empty);
  gtk_label_set_text (GTK_LABEL (self->albums_status), out == 0 ? empty : "");
}

void
spotifygtk_library_page_set_playlists (SpotifyGtkLibraryPage *self,
                                        const SpotifyGtkCardSpec *cards,
                                        guint n_cards)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));
  g_autoptr(GPtrArray) next = g_ptr_array_new_with_free_func (library_playlist_free);
  for (guint i = 0; cards && i < n_cards; i++) {
    if (!cards[i].uri) continue;
    SpotifyGtkCardSpec *card = g_new0 (SpotifyGtkCardSpec, 1);
    card->uri = g_strdup (cards[i].uri);
    card->title = g_strdup (cards[i].title);
    card->subtitle = g_strdup (cards[i].subtitle);
    card->cover_id = g_strdup (cards[i].cover_id);
    card->added_at = cards[i].added_at;
    g_ptr_array_add (next, card);
  }
  g_clear_pointer (&self->playlists, g_ptr_array_unref);
  self->playlists = g_steal_pointer (&next);
  if (!self->separate_playlists &&
      (self->active_view == LIBRARY_ALL ||
       self->active_view == LIBRARY_PLAYLISTS))
    show_release_view (self);
}

void
spotifygtk_library_page_resolve_playlist (SpotifyGtkLibraryPage *self,
  const gchar *uri, const gchar *name, const gchar *cover)
{
  for (guint i = 0; self->playlists && i < self->playlists->len; i++) {
    SpotifyGtkCardSpec *card = self->playlists->pdata[i];
    if (g_strcmp0 (card->uri, uri) != 0) continue;
    g_free ((gchar *) card->title); card->title = g_strdup (name);
    g_free ((gchar *) card->cover_id); card->cover_id = g_strdup (cover);
    break;
  }
  spotifygtk_album_grid_resolve_card (self->albums_primary, uri, name, "Playlist", cover);
  spotifygtk_album_grid_resolve_card (self->albums_alt, uri, name, "Playlist", cover);
}

static void
on_library_settings_changed (SpotifyGtkSettings *settings,
                             SpotifyGtkLibraryPage *self)
{
  gboolean separate = spotifygtk_settings_get_show_playlists_separately (settings);
  if (separate == self->separate_playlists)
    return;
  self->separate_playlists = separate;
  gtk_widget_set_visible (self->view_buttons[LIBRARY_PLAYLISTS], !separate);
  gtk_widget_set_visible (self->new_playlist_button, !separate);
  if (separate && self->active_view == LIBRARY_PLAYLISTS)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (
      self->view_buttons[LIBRARY_ALL]), TRUE);
  else if (self->active_view == LIBRARY_ALL)
    show_release_view (self);
}

static void
on_album_meta_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LibLoad *cl = user_data;
  g_autoptr(SpotifyGtkLibraryPage) self = g_weak_ref_get (&cl->page);
  guint cl_generation = cl->generation;
  g_weak_ref_clear (&cl->page);
  g_free (cl);

  g_autoptr(GError) err = NULL;
  g_autoptr(GPtrArray) albums = spotifygtk_native_session_load_albums_finish (
    SPOTIFYGTK_NATIVE_SESSION (source), result, &err);
  spotifygtk_runtime_schedule_heap_trim ();

  if (!self)
    return;
  if (cl_generation != self->generation)
    return;   /* a newer load has already replaced this one */
  g_clear_handle_id (&self->load_timeout_id, g_source_remove);
  g_clear_object (&self->load_cancel);
  set_loading (self, FALSE);
  if (!albums) {
    if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      gtk_label_set_text (GTK_LABEL (self->albums_status),
                          "Could not load your albums right now.");
      gtk_widget_set_visible (self->albums_status, TRUE);
    }
    return;
  }
  self->loaded = TRUE;
  self->load_retry_count = 0;

  g_clear_pointer (&self->saved_releases, g_ptr_array_unref);
  self->saved_releases = g_steal_pointer (&albums);
  sort_releases_by_date_added (self);
  show_release_view (self);
}

/* Every album URI in the collection set. Liked tracks share that set and are
 * skipped here by prefix. */
static gboolean
read_saved_page (SpotifyGtkLibraryPage *self, const gchar *token);

static void
on_saved_page (gboolean ok, guint16 status, SpotifyCollectionItem *items,
               guint n_items, const gchar *next_token, gpointer user_data)
{
  LibLoad *cl = user_data;
  g_autoptr(SpotifyGtkLibraryPage) self = g_weak_ref_get (&cl->page);
  guint mine = cl->generation;
  g_weak_ref_clear (&cl->page);
  g_free (cl);

  /* Mercury replies cannot be cancelled at the transport layer. A session
   * replacement cancels and clears this marker, so discard its late pages
   * instead of continuing the old read against the new session. */
  if (!self || mine != self->generation || !self->load_cancel ||
      g_cancellable_is_cancelled (self->load_cancel))
    return;

  if (!ok) {
    g_clear_handle_id (&self->load_timeout_id, g_source_remove);
    g_clear_object (&self->load_cancel);
    set_loading (self, FALSE);
    gtk_label_set_text (GTK_LABEL (self->albums_status),
                        "Could not read your library right now.");
    gtk_widget_set_visible (self->albums_status, TRUE);
    g_message ("library: saved-album read failed (status %u)", status);
    return;
  }

  if (!self->saved_album_uris)
    self->saved_album_uris = g_ptr_array_new_with_free_func (g_free);
  if (!self->saved_album_dates)
    self->saved_album_dates = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, NULL);

  for (guint i = 0; i < n_items; i++) {
    if (!items[i].uri || items[i].is_removed)
      continue;
    if (g_str_has_prefix (items[i].uri, "spotify:album:")) {
      g_ptr_array_add (self->saved_album_uris, g_strdup (items[i].uri));
      g_hash_table_replace (self->saved_album_dates, g_strdup (items[i].uri),
                            GINT_TO_POINTER (items[i].added_at));
    }
  }

  if (next_token && *next_token) {
    if (read_saved_page (self, next_token))
      return;
    g_clear_handle_id (&self->load_timeout_id, g_source_remove);
    g_clear_object (&self->load_cancel);
    set_loading (self, FALSE);
    gtk_label_set_text (GTK_LABEL (self->albums_status),
                        "Library connection was lost. Reopen it to retry.");
    gtk_widget_set_visible (self->albums_status, TRUE);
    return;
  }

  cl = g_new0 (LibLoad, 1);
  g_weak_ref_init (&cl->page, self);
  cl->generation = mine;
  arm_library_timeout (self);
  spotifygtk_native_session_load_albums (self->session,
    (const gchar *const *) self->saved_album_uris->pdata,
    self->saved_album_uris->len, self->load_cancel, on_album_meta_loaded, cl);
}

static gboolean
read_saved_page (SpotifyGtkLibraryPage *self, const gchar *token)
{
  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  g_autofree gchar *user = m
    ? spotifygtk_native_session_dup_username (self->session) : NULL;
  if (!m || !user)
    return FALSE;

  LibLoad *cl = g_new0 (LibLoad, 1);
  g_weak_ref_init (&cl->page, self);
  cl->generation = self->generation;
  arm_library_timeout (self);
  spotifygtk_collection_v2_read_page (m, user, SPOTIFYGTK_COLLECTION_SET_LIKED,
                                      token, 500, on_saved_page, cl);
  return TRUE;
}

void
spotifygtk_library_page_set_session (SpotifyGtkLibraryPage *self,
                                     SpotifyNativeSession  *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));

  /* READY is emitted again after an access-point reconnect.  It is still the
   * same session and the already resolved album model remains valid; clearing
   * it here discarded every cached name/cover association and started the
   * expensive collection load again in the middle of normal UI activity. */
  if (self->session == session)
    return;

  self->session = session;
  self->generation++;
  g_clear_handle_id (&self->load_timeout_id, g_source_remove);
  self->load_retry_count = 0;

  if (self->load_cancel) {
    g_cancellable_cancel (self->load_cancel);
    g_clear_object (&self->load_cancel);
  }
  set_loading (self, FALSE);
  self->loaded = FALSE;
  g_clear_pointer (&self->saved_releases, g_ptr_array_unref);
  g_clear_pointer (&self->saved_album_uris, g_ptr_array_unref);
  g_clear_pointer (&self->saved_album_dates, g_hash_table_unref);
}

static void
start_library_load (SpotifyGtkLibraryPage *self)
{
  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY)
    return;

  gtk_label_set_text (GTK_LABEL (self->albums_status), "");
  gtk_widget_set_visible (self->albums_status, FALSE);
  set_loading (self, TRUE);

  self->load_cancel = g_cancellable_new ();
  self->generation++;
  g_clear_pointer (&self->saved_album_uris, g_ptr_array_unref);
  g_clear_pointer (&self->saved_album_dates, g_hash_table_unref);
  if (!read_saved_page (self, NULL)) {
    g_clear_object (&self->load_cancel);
    set_loading (self, FALSE);
  }
}

void
spotifygtk_library_page_refresh (SpotifyGtkLibraryPage *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));
  /* Lazy first visit, with a clear retry path after a failed request. */
  if (self->loaded || self->load_cancel)
    return;
  self->load_retry_count = 0;
  start_library_load (self);
}

SpotifyGtkAlbumGrid *
spotifygtk_library_page_get_album_grid (SpotifyGtkLibraryPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self), NULL);
  return self->albums;
}

SpotifyGtkAlbumGrid *
spotifygtk_library_page_get_alt_album_grid (SpotifyGtkLibraryPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self), NULL);
  return self->albums_alt;
}

SpotifyGtkAlbumGrid *
spotifygtk_library_page_get_artist_grid (SpotifyGtkLibraryPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self), NULL);
  return self->artists;
}

typedef struct {
  GWeakRef page;
  gchar   *uri;
} SavedAlbumUpdate;

static void
saved_album_update_free (SavedAlbumUpdate *update)
{
  g_weak_ref_clear (&update->page);
  g_free (update->uri);
  g_free (update);
}

static gint
saved_album_release_index (SpotifyGtkLibraryPage *self, const gchar *uri)
{
  if (!self->saved_releases)
    return -1;
  for (guint i = 0; i < self->saved_releases->len; i++) {
    const SpotifyNativeRelease *release =
      g_ptr_array_index (self->saved_releases, i);
    if (release && g_strcmp0 (release->uri, uri) == 0)
      return (gint) i;
  }
  return -1;
}

static void
on_saved_album_added_loaded (GObject *source, GAsyncResult *result,
                             gpointer user_data)
{
  SavedAlbumUpdate *update = user_data;
  g_autoptr(SpotifyGtkLibraryPage) self = g_weak_ref_get (&update->page);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) albums = spotifygtk_native_session_load_albums_finish (
    SPOTIFYGTK_NATIVE_SESSION (source), result, &error);

  if (!self || source != G_OBJECT (self->session) ||
      !self->saved_album_dates ||
      !g_hash_table_contains (self->saved_album_dates, update->uri)) {
    saved_album_update_free (update);
    return;
  }

  if (!albums || albums->len == 0) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("library: saved album metadata failed for %s: %s",
                 update->uri, error ? error->message : "empty response");
    saved_album_update_free (update);
    return;
  }

  SpotifyNativeRelease *release = g_ptr_array_steal_index (albums, 0);
  gint old = saved_album_release_index (self, update->uri);
  if (old >= 0)
    g_ptr_array_remove_index (self->saved_releases, (guint) old);
  g_ptr_array_add (self->saved_releases, release);
  sort_releases_by_date_added (self);
  show_release_view (self);
  spotifygtk_runtime_schedule_heap_trim ();
  saved_album_update_free (update);
}

void
spotifygtk_library_page_set_album_saved (SpotifyGtkLibraryPage *self,
                                         const gchar           *uri,
                                         gboolean               saved)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));
  if (!uri || !g_str_has_prefix (uri, "spotify:album:"))
    return;

  if (!saved) {
    if (self->saved_album_dates)
      g_hash_table_remove (self->saved_album_dates, uri);
    if (self->saved_album_uris) {
      for (guint i = 0; i < self->saved_album_uris->len; i++) {
        if (g_strcmp0 (g_ptr_array_index (self->saved_album_uris, i), uri) == 0) {
          g_ptr_array_remove_index (self->saved_album_uris, i);
          break;
        }
      }
    }
    gint index = saved_album_release_index (self, uri);
    if (index >= 0)
      g_ptr_array_remove_index (self->saved_releases, (guint) index);
    spotifygtk_album_grid_remove_uri (self->albums, uri);
    if (self->loaded)
      show_release_view (self);
    spotifygtk_runtime_schedule_heap_trim ();
    return;
  }

  /* A page that has never loaded will naturally see the album in its first
   * collection read. Only patch an existing model. */
  if (!self->loaded || !self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY)
    return;

  if (!self->saved_album_uris)
    self->saved_album_uris = g_ptr_array_new_with_free_func (g_free);
  if (!self->saved_album_dates)
    self->saved_album_dates = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, NULL);
  if (!self->saved_releases)
    self->saved_releases = g_ptr_array_new_with_free_func (
      (GDestroyNotify) spotifygtk_native_release_free);

  if (!g_hash_table_contains (self->saved_album_dates, uri))
    g_ptr_array_add (self->saved_album_uris, g_strdup (uri));
  g_hash_table_replace (self->saved_album_dates, g_strdup (uri),
                        GINT_TO_POINTER ((gint) (g_get_real_time () /
                                                G_USEC_PER_SEC)));

  SavedAlbumUpdate *update = g_new0 (SavedAlbumUpdate, 1);
  g_weak_ref_init (&update->page, self);
  update->uri = g_strdup (uri);
  const gchar *uris[] = { uri };
  spotifygtk_native_session_load_albums (self->session, uris, 1, NULL,
                                         on_saved_album_added_loaded, update);
}

static void
populate_artist_cards (SpotifyGtkLibraryPage *self)
{
  if (self->artists_populated)
    return;
  self->artists_populated = TRUE;

  guint n = self->followed_artist_uris ? self->followed_artist_uris->len : 0;
  g_autofree SpotifyGtkCardSpec *specs = g_new0 (SpotifyGtkCardSpec, MAX (n, 1));
  g_autoptr(GPtrArray) identities = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < n; i++) {
    const gchar *uri = g_ptr_array_index (self->followed_artist_uris, i);
    specs[i].uri = uri;
    specs[i].title = "Artist";
    specs[i].subtitle = "Followed artist";
    gchar *name = NULL, *cover = NULL;
    if (spotifygtk_catalog_card_get (uri, &name, &cover)) {
      specs[i].title = name;
      specs[i].cover_id = cover;
      g_ptr_array_add (identities, name);
      g_ptr_array_add (identities, cover);
    }
  }
  spotifygtk_album_grid_set_pending_cards (self->artists, specs, n);
  gtk_widget_set_visible (self->artists_status, n == 0);
  gtk_label_set_text (GTK_LABEL (self->artists_status),
                      n == 0 ? "No followed artists yet." : "");
}

void
spotifygtk_library_page_set_followed_artists (SpotifyGtkLibraryPage *self,
                                              GHashTable            *uris)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));

  g_clear_pointer (&self->followed_artist_uris, g_ptr_array_unref);
  self->followed_artist_uris = g_ptr_array_new_with_free_func (g_free);
  if (uris) {
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init (&it, uris);
    while (g_hash_table_iter_next (&it, &key, NULL))
      if (g_str_has_prefix ((const gchar *) key, "spotify:artist:"))
        g_ptr_array_add (self->followed_artist_uris, g_strdup (key));
  }
  self->artists_populated = FALSE;
  if (self->content_stack &&
      g_strcmp0 (gtk_stack_get_visible_child_name (self->content_stack), "artists") == 0)
    populate_artist_cards (self);
}

typedef struct {
  GWeakRef page;
  guint generation;
  gchar *uri;
  gchar *name;
  gchar *cover_id;
  guint remaining;
} ArtistCardLoad;

static void
artist_card_load_complete (ArtistCardLoad *load)
{
  if (--load->remaining != 0)
    return;
  g_autoptr(SpotifyGtkLibraryPage) self = g_weak_ref_get (&load->page);
  if (self && load->generation == self->generation)
    spotifygtk_album_grid_resolve_card (self->artists, load->uri,
      load->name ? load->name : "Artist", "Artist", load->cover_id);
  g_weak_ref_clear (&load->page);
  g_free (load->uri);
  g_free (load->name);
  g_free (load->cover_id);
  g_free (load);
}

static void
on_artist_card_identity (const gchar *name, const gchar *cover_id,
                         gpointer user_data)
{
  ArtistCardLoad *load = user_data;
  if (name && *name) {
    g_free (load->name);
    load->name = g_strdup (name);
  }
  if (cover_id && *cover_id) {
    g_free (load->cover_id);
    load->cover_id = g_strdup (cover_id);
  }
  artist_card_load_complete (load);
}

static void
on_artist_card_needs_resolve (SpotifyGtkAlbumGrid *grid,
                              const gchar *uri,
                              gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;
  if (!self->session || !uri)
    return;
  ArtistCardLoad *load = g_new0 (ArtistCardLoad, 1);
  g_weak_ref_init (&load->page, self);
  load->generation = self->generation;
  load->uri = g_strdup (uri);
  spotifygtk_catalog_card_get (uri, &load->name, &load->cover_id);
  load->remaining = 1;
  spotifygtk_native_session_get_artist_identity (
    self->session, uri, on_artist_card_identity, load);
  (void) grid;
}

typedef struct {
  GWeakRef page;
  SpotifyGtkAlbumGrid *grid; /* borrowed while page lives */
} ReleaseFadeCleanup;

static gboolean
on_release_fade_finished (gpointer data)
{
  ReleaseFadeCleanup *cleanup = data;
  g_autoptr(SpotifyGtkLibraryPage) self = g_weak_ref_get (&cleanup->page);
  if (self && cleanup->grid != self->albums) {
    spotifygtk_album_grid_release_covers (cleanup->grid);
    spotifygtk_album_grid_clear (cleanup->grid);
  }
  return G_SOURCE_REMOVE;
}

static void
release_fade_cleanup_free (gpointer data)
{
  ReleaseFadeCleanup *cleanup = data;
  g_weak_ref_clear (&cleanup->page);
  g_free (cleanup);
}

static void
on_view_clicked (GtkToggleButton *button, gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;
  if (!gtk_toggle_button_get_active (button))
    return;
  LibraryView view = (LibraryView) GPOINTER_TO_UINT (
    g_object_get_data (G_OBJECT (button), "library-view"));
  gboolean artists = view == LIBRARY_ARTISTS;
  gboolean changing_release = !artists && self->active_view != LIBRARY_ARTISTS &&
                              view != self->active_view;
  SpotifyGtkAlbumGrid *outgoing = changing_release ? self->albums : NULL;
  self->active_view = view;

  /* GtkStack keeps the hidden child alive, including each card's texture.
   * Switching views must relinquish those widget references; the compressed
   * disk cache is deliberately retained and makes restoration inexpensive. */
  if (changing_release) {
    self->albums = outgoing == self->albums_primary
      ? self->albums_alt : self->albums_primary;
    self->albums_status = self->albums == self->albums_primary
      ? self->albums_status_primary : self->albums_status_alt;
    const gchar *filter = gtk_editable_get_text (GTK_EDITABLE (self->filter_entry));
    spotifygtk_album_grid_set_filter_text (self->albums, filter);
    show_release_view (self);
    gtk_stack_set_visible_child_name (self->content_stack,
      self->albums == self->albums_primary ? "releases" : "releases-alt");
    /* The old grid stays painted only during the fade. */
    ReleaseFadeCleanup *cleanup = g_new0 (ReleaseFadeCleanup, 1);
    g_weak_ref_init (&cleanup->page, self);
    cleanup->grid = outgoing;
    g_timeout_add_full (G_PRIORITY_LOW, 190, on_release_fade_finished,
                        cleanup, release_fade_cleanup_free);
  } else if (artists)
    spotifygtk_album_grid_release_covers (self->albums);
  else
    spotifygtk_album_grid_release_covers (self->artists);
  if (!changing_release) {
    gtk_stack_set_visible_child_name (self->content_stack,
      artists ? "artists" : self->albums == self->albums_primary
        ? "releases" : "releases-alt");
    if (artists) populate_artist_cards (self);
    else show_release_view (self);
  }
  spotifygtk_album_grid_reload_covers (artists ? self->artists : self->albums);
}

static void
on_local_catalog_changed (SpotifyGtkLocalCatalog *catalog, gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;
  SpotifyGtkLocalSnapshot *next = spotifygtk_local_catalog_ref_snapshot ();
  SpotifyGtkLocalSnapshot *old = self->local_snapshot;
  self->local_snapshot = next;
  if (self->active_view == LIBRARY_ALL || self->active_view == LIBRARY_LOCAL)
    show_release_view (self);
  spotifygtk_local_snapshot_unref (old);
  (void) catalog;
}

static void
on_filter_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  spotifygtk_album_grid_set_filter_text (self->albums, text);
  spotifygtk_album_grid_set_filter_text (
    self->albums == self->albums_primary ? self->albums_alt : self->albums_primary,
    text);
  spotifygtk_album_grid_set_filter_text (self->artists, text);
}

void
spotifygtk_library_page_set_covers_loaded (SpotifyGtkLibraryPage *self,
                                           gboolean               loaded)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));
  if (!loaded) {
    spotifygtk_album_grid_release_covers (self->albums);
    spotifygtk_album_grid_release_covers (self->albums == self->albums_primary
      ? self->albums_alt : self->albums_primary);
    spotifygtk_album_grid_release_covers (self->artists);
    return;
  }

  gboolean artists = self->content_stack &&
    g_strcmp0 (gtk_stack_get_visible_child_name (self->content_stack), "artists") == 0;
  spotifygtk_album_grid_reload_covers (artists ? self->artists : self->albums);
}

/* Fold the header with hysteresis. It is not the source of the grid stutter;
 * keeping this behavior also preserves the vertical space while browsing. */
#define HEADER_HIDE_AT   90.0
#define HEADER_SHOW_AT   12.0

static void
on_albums_scrolled (GtkAdjustment *adj, gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;
  const gchar *visible = gtk_stack_get_visible_child_name (self->content_stack);
  SpotifyGtkAlbumGrid *active = g_strcmp0 (visible, "artists") == 0
    ? self->artists : self->albums;
  if (adj != spotifygtk_album_grid_get_vadjustment (active))
    return;
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
add_local_empty_state (GtkStack *stack, GtkWidget *page, const gchar *name)
{
  GtkWidget *overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), page);
  GtkWidget *state = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_add_css_class (state, "local-files-empty-state");
  gtk_widget_set_halign (state, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (state, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (state, 24);
  gtk_widget_set_margin_end (state, 24);
  gtk_widget_set_can_target (state, FALSE);
  /* A non-symbolic scalable icon keeps the supplied colors and a bounded
   * 228px natural size, rather than the SVG's original 512px request. */
  GtkWidget *vinyl = gtk_image_new_from_icon_name ("spotifygtk-vinyl-record");
  gtk_image_set_pixel_size (GTK_IMAGE (vinyl), 228);
  gtk_widget_set_halign (vinyl, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (state), vinyl);
  GtkWidget *hint = gtk_label_new (
    "Enable Local files and add a folder to get started.");
  gtk_widget_add_css_class (hint, "dim-text");
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 42);
  gtk_label_set_justify (GTK_LABEL (hint), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (state), hint);
  gtk_widget_set_visible (state, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), state);
  gtk_stack_add_named (stack, overlay, name);
  return state;
}

static void
spotifygtk_library_page_init (SpotifyGtkLibraryPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* No outer scroller: the albums GridView brings its own, and nesting it in a
   * second vertical scroller would stop it virtualising. */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (header, 35);
  gtk_widget_set_margin_end (header, 35);
  gtk_widget_set_margin_top (header, 24);

  GtkWidget *title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *title = gtk_label_new ("Library");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_box_append (GTK_BOX (title_row), title);
  self->new_playlist_button = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_add_css_class (self->new_playlist_button, "playlist-create-button");
  gtk_widget_set_tooltip_text (self->new_playlist_button, "Create playlist");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->new_playlist_button),
    GTK_ACCESSIBLE_PROPERTY_LABEL, "Create playlist", -1);
  g_signal_connect (self->new_playlist_button, "clicked",
                    G_CALLBACK (on_new_playlist_clicked), self);
  gtk_box_append (GTK_BOX (header), title_row);

  /* Match the sort controls elsewhere: caption plus a linked segmented group,
   * pushed to the right edge of the content area. */
  GtkWidget *view_controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_bottom (view_controls, 4);

  self->filter_entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_search_entry_set_placeholder_text (self->filter_entry, "Filter library");
  gtk_widget_set_halign (GTK_WIDGET (self->filter_entry), GTK_ALIGN_START);
  gtk_widget_set_valign (GTK_WIDGET (self->filter_entry), GTK_ALIGN_CENTER);
  gtk_widget_set_size_request (GTK_WIDGET (self->filter_entry), 340, -1);
  g_signal_connect (self->filter_entry, "search-changed",
                    G_CALLBACK (on_filter_changed), self);
  gtk_box_append (GTK_BOX (view_controls), GTK_WIDGET (self->filter_entry));
  gtk_widget_set_valign (self->new_playlist_button, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (view_controls), self->new_playlist_button);

  GtkWidget *view_spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (view_spacer, TRUE);
  gtk_box_append (GTK_BOX (view_controls), view_spacer);

  GtkWidget *view_caption = gtk_label_new ("Sort by");
  gtk_widget_add_css_class (view_caption, "dim-text");
  gtk_widget_set_valign (view_caption, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (view_controls), view_caption);

  GtkWidget *view_group = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (view_group, "linked");
  gtk_widget_set_valign (view_group, GTK_ALIGN_CENTER);
  const gchar *view_names[] = {
    "All", "Albums", "EPs", "Singles", "Artists", "Local Files", "Playlists"
  };
  for (guint i = 0; i < N_LIBRARY_VIEWS; i++) {
    GtkWidget *button = gtk_toggle_button_new_with_label (view_names[i]);
    gtk_widget_add_css_class (button, "flat");
    g_object_set_data (G_OBJECT (button), "library-view", GUINT_TO_POINTER (i));
    if (i > 0)
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (button),
                                   GTK_TOGGLE_BUTTON (self->view_buttons[0]));
    g_signal_connect (button, "toggled", G_CALLBACK (on_view_clicked), self);
    self->view_buttons[i] = button;
    gtk_box_append (GTK_BOX (view_group), button);
  }
  gtk_box_append (GTK_BOX (view_controls), view_group);
  gtk_box_append (GTK_BOX (header), view_controls);

  self->header_revealer = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (self->header_revealer),
                                    GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_transition_duration (GTK_REVEALER (self->header_revealer), 180);
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_revealer), TRUE);
  gtk_revealer_set_child (GTK_REVEALER (self->header_revealer), header);
  gtk_box_append (GTK_BOX (self), self->header_revealer);

  self->content_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->content_stack,
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (self->content_stack, 160);
  /* Match the small breathing room between controls and results on Liked
   * Songs; without it the first card row touches the loading-rule edge. */
  gtk_widget_set_margin_top (GTK_WIDGET (self->content_stack), 10);
  gtk_widget_set_vexpand (GTK_WIDGET (self->content_stack), TRUE);

  GtkWidget *albums_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  self->albums_status = gtk_label_new ("Not signed in yet.");
  self->albums_status_primary = self->albums_status;
  gtk_widget_add_css_class (self->albums_status, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (self->albums_status), 0.0);
  gtk_widget_set_margin_start (self->albums_status, 35);
  gtk_box_append (GTK_BOX (albums_page), self->albums_status);

  self->albums = spotifygtk_album_grid_new_grid ();
  self->albums_primary = self->albums;
  /*
   * The horizontal inset goes on the cards, not on this widget. A margin here
   * would push the scrollbar inward too, stacking its width on top of the
   * margin and leaving a dead gutter beside it -- the bar belongs flush with
   * the page edge like every other scroller in the app. The end inset is a
   * little smaller than the start because the bar itself occupies the
   * difference, which is what makes the two sides read as equal.
   */
  spotifygtk_album_grid_set_content_margins (self->albums, 35, 2);
  gtk_widget_set_vexpand (GTK_WIDGET (self->albums), TRUE);
  gtk_box_append (GTK_BOX (albums_page), GTK_WIDGET (self->albums));
  self->local_empty_primary = add_local_empty_state (
    self->content_stack, albums_page, "releases");

  GtkWidget *albums_alt_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  self->albums_status_alt = gtk_label_new ("");
  gtk_widget_add_css_class (self->albums_status_alt, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (self->albums_status_alt), 0.0);
  gtk_widget_set_margin_start (self->albums_status_alt, 35);
  gtk_widget_set_visible (self->albums_status_alt, FALSE);
  gtk_box_append (GTK_BOX (albums_alt_page), self->albums_status_alt);
  self->albums_alt = spotifygtk_album_grid_new_grid ();
  spotifygtk_album_grid_set_content_margins (self->albums_alt, 35, 2);
  gtk_widget_set_vexpand (GTK_WIDGET (self->albums_alt), TRUE);
  gtk_box_append (GTK_BOX (albums_alt_page), GTK_WIDGET (self->albums_alt));
  self->local_empty_alt = add_local_empty_state (
    self->content_stack, albums_alt_page, "releases-alt");

  GtkWidget *artists_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  self->artists_status = gtk_label_new ("");
  gtk_widget_add_css_class (self->artists_status, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (self->artists_status), 0.0);
  gtk_widget_set_margin_start (self->artists_status, 35);
  gtk_widget_set_visible (self->artists_status, FALSE);
  gtk_box_append (GTK_BOX (artists_page), self->artists_status);
  self->artists = spotifygtk_album_grid_new_grid ();
  spotifygtk_album_grid_set_content_margins (self->artists, 35, 2);
  gtk_widget_set_vexpand (GTK_WIDGET (self->artists), TRUE);
  g_signal_connect (self->artists, "card-needs-resolve",
                    G_CALLBACK (on_artist_card_needs_resolve), self);
  gtk_box_append (GTK_BOX (artists_page), GTK_WIDGET (self->artists));
  gtk_stack_add_named (self->content_stack, artists_page, "artists");
  gtk_stack_set_visible_child_name (self->content_stack, "releases");
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->content_stack));
  self->active_view = LIBRARY_ALL;
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  self->separate_playlists =
    spotifygtk_settings_get_show_playlists_separately (settings);
  gtk_widget_set_visible (self->view_buttons[LIBRARY_PLAYLISTS],
                          !self->separate_playlists);
  gtk_widget_set_visible (self->new_playlist_button,
                          !self->separate_playlists);
  g_signal_connect_object (settings, "changed",
                           G_CALLBACK (on_library_settings_changed), self, 0);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->view_buttons[0]), TRUE);

  SpotifyGtkLocalCatalog *catalog = spotifygtk_local_catalog_get_default ();
  self->local_snapshot = spotifygtk_local_catalog_ref_snapshot ();
  g_signal_connect_object (catalog, "changed",
                           G_CALLBACK (on_local_catalog_changed), self, 0);
  show_release_view (self);

  GtkAdjustment *vadj = spotifygtk_album_grid_get_vadjustment (self->albums);
  if (vadj)
    g_signal_connect (vadj, "value-changed", G_CALLBACK (on_albums_scrolled), self);
  vadj = spotifygtk_album_grid_get_vadjustment (self->albums_alt);
  if (vadj)
    g_signal_connect (vadj, "value-changed", G_CALLBACK (on_albums_scrolled), self);
  vadj = spotifygtk_album_grid_get_vadjustment (self->artists);
  if (vadj)
    g_signal_connect (vadj, "value-changed", G_CALLBACK (on_albums_scrolled), self);
}

SpotifyGtkLibraryPage *
spotifygtk_library_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_LIBRARY_PAGE, NULL);
}

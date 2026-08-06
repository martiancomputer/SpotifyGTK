/*
 * liked_songs_page.c — Liked Songs page implementation.
 */

#include "liked_songs_page.h"
#include "track_list.h"

#include <string.h>

/* A real collection runs to thousands of tracks (4,773 on the account this
 * was developed against) and a single metadata batch that large is rejected
 * by the server in one request. The session now pages: it splits a load into
 * MAX_BATCH-sized metadata requests and concatenates them, so this asks for the
 * whole-load ceiling and lets that happen underneath. */
#define LIKED_SONGS_LIMIT SPOTIFYGTK_SESSION_MAX_TRACKS

/* After a failure, ignore refresh requests for this long, so revisiting the
 * page cannot turn one error into a stream of retries. */
#define RETRY_COOLDOWN_US (10 * G_USEC_PER_SEC)

struct _SpotifyGtkLikedSongsPage {
  GtkBox parent_instance;

  SpotifyGtkTrackList  *list;
  GtkSearchEntry       *filter_entry;
  SpotifyNativeSession *session;
  GCancellable         *in_flight;

  /* The full loaded collection, kept so the filter can rebuild the visible
   * list from it without re-fetching. Owned (a ref on the loaded array). */
  GPtrArray *all_tracks;

  /* Borrowed from the window: URIs known to be liked, read from collection v2.
   * Authoritative; see set_liked_filter(). */
  GHashTable *liked_filter;

  /*
   * Filtering and sorting precomputation, both keyed by track pointer so the
   * sort order can change without invalidating them.
   *
   * Built once when the collection loads. Doing this per keystroke instead --
   * which is what it used to do -- meant a casefold and two allocations for
   * every one of ~4800 tracks on every character typed, plus two more casefolds
   * per comparison inside the sort. That is the stutter.
   *
   * Collate keys rather than casefolded names for the A-Z sort: g_utf8_collate()
   * has to build one internally on every comparison, so precomputing turns each
   * comparison into a strcmp.
   */
  GHashTable *haystacks;     /* track -> casefolded "name\tartists\talbum" */
  GHashTable *collate_keys;  /* track -> g_utf8_collate_key of the title */

  /* all_tracks in the current sort order, borrowed pointers. Rebuilt when the
   * sort changes, not when the filter text does -- the order does not depend on
   * the query. */
  GPtrArray *sorted_tracks;

  /* Sort controls: which key, and whether it runs the natural way round. */
  GtkWidget *sort_buttons[3];
  guint      sort_key;
  gboolean   sort_desc[3];   /* remembered per key, so switching back restores it */

  gboolean loaded;
  gint64   retry_after;
};

/*
 * Sort keys.
 *
 * ADDED is the order the collection came back in, which is Spotify's own
 * newest-first ordering -- so it needs no key of its own, just the original
 * index. That matters because the actual added_at timestamp lives in
 * collection2v2, which this client has a write encoder for and no read path;
 * sorting by position gets the ordering right without that work, though it is
 * why no date can be *shown* per row yet.
 */
enum {
  SORT_ADDED = 0,
  SORT_LENGTH,
  SORT_ALPHA,
  N_SORT_KEYS
};

static const gchar *const SORT_LABELS[N_SORT_KEYS] = {
  "Date added", "Length", "A\u2013Z"
};

G_DEFINE_FINAL_TYPE (SpotifyGtkLikedSongsPage, spotifygtk_liked_songs_page, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
on_track_activated (SpotifyGtkTrackList *list, gpointer track, gpointer user_data)
{
  SpotifyGtkLikedSongsPage *self = user_data;
  g_signal_emit (self, signals[TRACK_ACTIVATED], 0, track);
  (void) list;
}

/* A track matches when every whitespace-separated term of the (casefolded)
 * query appears somewhere in its title, artists or album. The filter is local:
 * it narrows the already-loaded collection, so it is instant and works offline
 * of the catalog. */
static gboolean
track_matches (SpotifyGtkLikedSongsPage *self,
               const SpotifyNativeTrack *t, const gchar *const *terms)
{
  const gchar *hay_cf = self->haystacks ? g_hash_table_lookup (self->haystacks, t) : NULL;
  if (!hay_cf)
    return FALSE;

  for (guint i = 0; terms[i]; i++)
    if (!strstr (hay_cf, terms[i]))    /* an empty term matches, which is fine */
      return FALSE;
  return TRUE;
}

/* Build the per-track lookups. Called once per collection load. */
static void
build_track_indexes (SpotifyGtkLikedSongsPage *self)
{
  g_clear_pointer (&self->haystacks, g_hash_table_unref);
  g_clear_pointer (&self->collate_keys, g_hash_table_unref);
  if (!self->all_tracks)
    return;

  self->haystacks    = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  self->collate_keys = g_hash_table_new_full (NULL, NULL, NULL, g_free);

  for (guint i = 0; i < self->all_tracks->len; i++) {
    SpotifyNativeTrack *t = g_ptr_array_index (self->all_tracks, i);
    g_autofree gchar *hay = g_strdup_printf ("%s\t%s\t%s",
      t->name ? t->name : "", t->artists ? t->artists : "", t->album ? t->album : "");
    g_hash_table_insert (self->haystacks, t, g_utf8_casefold (hay, -1));
    g_hash_table_insert (self->collate_keys, t,
                         g_utf8_collate_key (t->name ? t->name : "", -1));
  }
}

/* Stable across equal keys: qsort is not required to be, and two songs of the
 * same length jumping about on every re-sort looks like a bug. The original
 * index is the tiebreak, which is also what makes SORT_ADDED work. */
typedef struct { SpotifyNativeTrack *track; guint index; } SortRow;

static gint
compare_rows (gconstpointer a, gconstpointer b, gpointer user_data)
{
  const SortRow *ra = a, *rb = b;
  SpotifyGtkLikedSongsPage *self = user_data;
  gint cmp = 0;

  switch (self->sort_key) {
  case SORT_LENGTH:
    cmp = (ra->track->duration_ms > rb->track->duration_ms) -
          (ra->track->duration_ms < rb->track->duration_ms);
    break;
  case SORT_ALPHA: {
    const gchar *ka = g_hash_table_lookup (self->collate_keys, ra->track);
    const gchar *kb = g_hash_table_lookup (self->collate_keys, rb->track);
    cmp = g_strcmp0 (ka, kb);
    break;
  }
  case SORT_ADDED:
  default:
    break;   /* index alone; see the enum comment */
  }

  if (cmp == 0)
    cmp = (ra->index > rb->index) - (ra->index < rb->index);
  else if (self->sort_desc[self->sort_key])
    cmp = -cmp;

  return cmp;
}

/* Sort `rows` (borrowed track pointers) in place under the current key. */
static void
sort_visible (SpotifyGtkLikedSongsPage *self, GPtrArray *rows)
{
  g_autoptr(GArray) tmp = g_array_sized_new (FALSE, FALSE, sizeof (SortRow), rows->len);
  for (guint i = 0; i < rows->len; i++) {
    SortRow r = { g_ptr_array_index (rows, i), i };
    g_array_append_val (tmp, r);
  }

  g_array_sort_with_data (tmp, compare_rows, self);

  for (guint i = 0; i < rows->len; i++)
    rows->pdata[i] = g_array_index (tmp, SortRow, i).track;
}

/* Rebuild sorted_tracks for the current key. Called when the sort changes or
 * the collection loads -- never from the filter, whose result is a subset of
 * this order rather than a different one. */
static void
rebuild_sorted (SpotifyGtkLikedSongsPage *self)
{
  g_clear_pointer (&self->sorted_tracks, g_ptr_array_unref);
  if (!self->all_tracks)
    return;

  self->sorted_tracks = g_ptr_array_sized_new (self->all_tracks->len);
  for (guint i = 0; i < self->all_tracks->len; i++)
    g_ptr_array_add (self->sorted_tracks, g_ptr_array_index (self->all_tracks, i));

  sort_visible (self, self->sorted_tracks);
}

/* Rebuild the visible list from all_tracks under the current filter text. */
static void
apply_filter (SpotifyGtkLikedSongsPage *self)
{
  if (!self->all_tracks)
    return;

  const gchar *query = gtk_editable_get_text (GTK_EDITABLE (self->filter_entry));

  if (!self->sorted_tracks)
    rebuild_sorted (self);

  if (!query || !*query) {
    spotifygtk_track_list_set_native_tracks (self->list, self->sorted_tracks);
    if (self->sorted_tracks->len == 0)
      spotifygtk_track_list_set_status (self->list, "No liked songs yet.");
    return;
  }

  g_autofree gchar *cf = g_utf8_casefold (query, -1);
  g_auto(GStrv) terms = g_strsplit (cf, " ", -1);

  /* Borrowed pointers into all_tracks; set_native_tracks takes its own copies,
   * so this shallow array needs no free func. */
  g_autoptr(GPtrArray) filtered = g_ptr_array_new ();
  for (guint i = 0; i < self->sorted_tracks->len; i++) {
    SpotifyNativeTrack *t = g_ptr_array_index (self->sorted_tracks, i);
    if (track_matches (self, t, (const gchar *const *) terms))
      g_ptr_array_add (filtered, t);
  }

  /* Already in sort order: filtering a sorted list preserves it. */
  spotifygtk_track_list_set_native_tracks (self->list, filtered);
  if (filtered->len == 0)
    spotifygtk_track_list_set_status (self->list, "No matches.");
}

/* Show the direction on the active button only. The inactive ones carry no
 * arrow, so the row reads as "sorted by this, that way round" at a glance
 * rather than as three independent controls. */
static void
refresh_sort_labels (SpotifyGtkLikedSongsPage *self)
{
  for (guint i = 0; i < N_SORT_KEYS; i++) {
    if (i == self->sort_key) {
      g_autofree gchar *text =
        g_strdup_printf ("%s %s", SORT_LABELS[i],
                         self->sort_desc[i] ? "\u2193" : "\u2191");
      gtk_button_set_label (GTK_BUTTON (self->sort_buttons[i]), text);
    } else {
      gtk_button_set_label (GTK_BUTTON (self->sort_buttons[i]), SORT_LABELS[i]);
    }
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->sort_buttons[i]),
                                  i == self->sort_key);
  }
}

static void
on_sort_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkLikedSongsPage *self = user_data;
  guint which = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "sort-key"));

  /* Clicking the active key flips it; clicking another switches to it and
   * keeps whichever direction that key was last left in. */
  if (which == self->sort_key)
    self->sort_desc[which] = !self->sort_desc[which];
  else
    self->sort_key = which;

  refresh_sort_labels (self);
  rebuild_sorted (self);
  apply_filter (self);
}

static void
on_filter_changed (GtkSearchEntry *entry, gpointer user_data)
{
  apply_filter (user_data);
  (void) entry;
}

static void
on_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  GWeakRef             *ref     = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(SpotifyGtkLikedSongsPage) self = g_weak_ref_get (ref);
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
    g_autofree gchar *msg = g_strdup_printf ("Couldn't load liked songs: %s", err->message);
    spotifygtk_track_list_clear (self->list);
    spotifygtk_track_list_set_status (self->list, msg);
    self->retry_after = g_get_monotonic_time () + RETRY_COOLDOWN_US;
    return;
  }

  self->retry_after = 0;

  /* Held across the rebuild below so rows this listing has not caught up on can
   * be carried over. See the carry-over loop. */
  g_autoptr(GPtrArray) previous = self->all_tracks;
  self->all_tracks = NULL;

  /*
   * Drop anything the liked set says is not liked. NULL until the read has
   * completed, so an empty set here means nothing is liked rather than nothing
   * is known -- which matters, because unliking the last track is precisely
   * when a stale refetch would put it back.
   */
  if (self->liked_filter) {
    GPtrArray *kept = g_ptr_array_new_with_free_func (
      (GDestroyNotify) spotifygtk_native_track_free);
    g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
    guint dropped = 0;
    for (guint i = 0; i < tracks->len; i++) {
      const SpotifyNativeTrack *t = g_ptr_array_index (tracks, i);
      if (t && t->uri && g_hash_table_contains (self->liked_filter, t->uri)) {
        g_ptr_array_add (kept, spotifygtk_native_track_copy (t));
        g_hash_table_add (seen, t->uri);
      } else {
        dropped++;
      }
    }
    if (dropped)
      g_message ("liked-page: dropped %u stale row(s) the collection no longer lists",
                 dropped);

    /*
     * The mirror image of that drop: keep rows this listing has not caught up
     * on yet.
     *
     * A track liked a moment ago is in the liked set immediately but reaches
     * the listing service later, and the listing may also be served from the
     * session's cache, which predates the write entirely. Without this, a
     * just-liked row appeared, survived until the user navigated away, and
     * then vanished on the way back -- which reads as the like having failed.
     *
     * Guarded by the liked set, so this can only reinstate something still
     * genuinely liked; once the listing catches up the row is already in
     * `seen` and this does nothing.
     */
    guint carried = 0;
    for (guint i = 0; previous && i < previous->len; i++) {
      const SpotifyNativeTrack *t = g_ptr_array_index (previous, i);
      if (!t || !t->uri)
        continue;
      if (g_hash_table_contains (seen, t->uri))
        continue;
      if (!g_hash_table_contains (self->liked_filter, t->uri))
        continue;
      /* At `carried`, not at 0: these are already newest-first in `previous`,
       * and inserting each at the front would reverse them. */
      g_ptr_array_insert (kept, (gint) carried, spotifygtk_native_track_copy (t));
      carried++;
    }
    if (carried)
      g_message ("liked-page: carried over %u row(s) the listing has not caught up on",
                 carried);

    self->all_tracks = kept;
  } else {
    self->all_tracks = g_ptr_array_ref (tracks);
  }
  /* Both derived from the tracks, so they are built here and not touched again
   * until the collection reloads. */
  build_track_indexes (self);
  rebuild_sorted (self);
  apply_filter (self);   /* honours whatever is already typed (usually nothing) */

  self->loaded = TRUE;
}

static void
spotifygtk_liked_songs_page_dispose (GObject *object)
{
  SpotifyGtkLikedSongsPage *self = SPOTIFYGTK_LIKED_SONGS_PAGE (object);

  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  g_clear_object (&self->session);
  g_clear_pointer (&self->all_tracks, g_ptr_array_unref);
  g_clear_pointer (&self->sorted_tracks, g_ptr_array_unref);
  g_clear_pointer (&self->haystacks, g_hash_table_unref);
  g_clear_pointer (&self->collate_keys, g_hash_table_unref);

  G_OBJECT_CLASS (spotifygtk_liked_songs_page_parent_class)->dispose (object);
}

static void
spotifygtk_liked_songs_page_class_init (SpotifyGtkLikedSongsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_liked_songs_page_dispose;

  signals[TRACK_ACTIVATED] = g_signal_new ("track-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
spotifygtk_liked_songs_page_init (SpotifyGtkLikedSongsPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 24);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  GtkWidget *title = gtk_label_new ("Liked Songs");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (self), title);

  /* Local filter over the loaded collection — narrows what's shown, does not
   * hit the catalog. */
  self->filter_entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_search_entry_set_placeholder_text (self->filter_entry, "Filter liked songs");
  gtk_widget_set_halign (GTK_WIDGET (self->filter_entry), GTK_ALIGN_START);
  gtk_widget_set_valign (GTK_WIDGET (self->filter_entry), GTK_ALIGN_CENTER);
  gtk_widget_set_size_request (GTK_WIDGET (self->filter_entry), 340, -1);
  g_signal_connect (self->filter_entry, "search-changed",
                    G_CALLBACK (on_filter_changed), self);

  /*
   * Filter on the left, sort on the right, one row.
   *
   * A "linked" box of toggle buttons is the stock GTK/libadwaita idiom for a
   * mutually exclusive set -- it renders as one segmented control using the
   * theme's own styling, so it inherits whatever the rest of the app is
   * wearing instead of introducing a look of its own.
   */
  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_bottom (controls, 4);
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->filter_entry));

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (controls), spacer);

  GtkWidget *sort_caption = gtk_label_new ("Sort by");
  gtk_widget_add_css_class (sort_caption, "dim-text");
  gtk_widget_set_valign (sort_caption, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (controls), sort_caption);

  GtkWidget *sort_group = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (sort_group, "linked");
  gtk_widget_set_valign (sort_group, GTK_ALIGN_CENTER);

  for (guint i = 0; i < N_SORT_KEYS; i++) {
    GtkWidget *b = gtk_toggle_button_new_with_label (SORT_LABELS[i]);
    gtk_widget_add_css_class (b, "flat");
    g_object_set_data (G_OBJECT (b), "sort-key", GUINT_TO_POINTER (i));
    g_signal_connect (b, "clicked", G_CALLBACK (on_sort_clicked), self);
    gtk_box_append (GTK_BOX (sort_group), b);
    self->sort_buttons[i] = b;
  }
  gtk_box_append (GTK_BOX (controls), sort_group);
  gtk_box_append (GTK_BOX (self), controls);

  /* Newest first is what the collection already arrives as, so the default
   * state describes the list rather than re-ordering it on load. */
  self->sort_key        = SORT_ADDED;
  self->sort_desc[SORT_ADDED] = FALSE;
  refresh_sort_labels (self);

  self->list = spotifygtk_track_list_new ();

  /* This page IS the liked set, so a heart on every row is noise -- see
   * spotifygtk_track_list_set_show_like(). */
  spotifygtk_track_list_set_show_like (self->list, FALSE);
  spotifygtk_track_list_set_numbered (self->list, TRUE);
  g_signal_connect (self->list, "track-activated", G_CALLBACK (on_track_activated), self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->list));
}

SpotifyGtkLikedSongsPage *
spotifygtk_liked_songs_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_LIKED_SONGS_PAGE, NULL);
}

void
spotifygtk_liked_songs_page_set_session (SpotifyGtkLikedSongsPage *self,
                                         SpotifyNativeSession     *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));

  g_clear_object (&self->session);
  self->session = session ? g_object_ref (session) : NULL;

  /* A newly-ready session is exactly what fixes an earlier failure. */
  self->loaded = FALSE;
  self->retry_after = 0;
}

/*
 * Force the next refresh to refetch.
 *
 * refresh() deliberately no-ops once the page holds data, so visiting it
 * repeatedly does not re-download the collection. That is right for
 * navigation and wrong after a like: the page kept showing the set as it was
 * at sign-in, and only a restart revealed a track added since.
 */

/*
 * The set of URIs actually liked, owned by the window and borrowed here.
 *
 * This page is populated from /context-resolve, which lags collection v2 by
 * around a second after a write -- long enough that a refetch triggered by the
 * change event returns the track that was just removed and puts the row back.
 * That is the "it vanishes then reappears" flicker.
 *
 * Filtering the fetched list through the set fixes it at the source rather
 * than by delaying the refetch, which would only make the race less likely.
 * The set comes from collection v2, is updated the moment the user acts, and
 * is never behind.
 *
 * Removal only: a track liked elsewhere is in the set but has no metadata here
 * yet, and appears on the next fetch.
 */
void
spotifygtk_liked_songs_page_set_liked_filter (SpotifyGtkLikedSongsPage *self,
                                              GHashTable *liked_uris)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));
  self->liked_filter = liked_uris;
}

void
spotifygtk_liked_songs_page_invalidate (SpotifyGtkLikedSongsPage *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));
  self->loaded      = FALSE;
  self->retry_after = 0;

  /*
   * Drop the session's cached listing too, or this promises a refetch it
   * cannot deliver: the collection is cached for the life of the session, so
   * the "refetch" was served from memory and returned the same set as before.
   * Marking the page stale then achieved nothing at all.
   */
  if (self->session) {
    g_autofree gchar *uri =
      spotifygtk_native_session_dup_collection_uri (self->session);
    if (uri)
      spotifygtk_native_session_invalidate_context (self->session, uri);
  }
}

/*
 * Add and remove a single track without refetching.
 *
 * Both go through all_tracks rather than the visible list, because the visible
 * list is a derived view: it is rebuilt from all_tracks on every keystroke in
 * the filter box and on every sort change. Touching only the rows meant an
 * unliked track came back the moment the user typed anything.
 *
 * The whole view is then rebuilt, which sounds heavy for one row and is not:
 * it is the same single splice that clearing the filter already does, and the
 * list is numbered, so inserting at the top renumbers everything below it
 * anyway.
 */
static gint
find_track_index (SpotifyGtkLikedSongsPage *self, const gchar *uri)
{
  if (!self->all_tracks || !uri)
    return -1;
  for (guint i = 0; i < self->all_tracks->len; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (self->all_tracks, i);
    if (t && t->uri && g_strcmp0 (t->uri, uri) == 0)
      return (gint) i;
  }
  return -1;
}

/* Index entries for one track, matching what build_track_indexes() produces. */
static void
index_one_track (SpotifyGtkLikedSongsPage *self, SpotifyNativeTrack *t)
{
  if (!self->haystacks || !self->collate_keys)
    return;

  g_autofree gchar *hay = g_strdup_printf ("%s\t%s\t%s",
    t->name ? t->name : "", t->artists ? t->artists : "", t->album ? t->album : "");
  g_hash_table_insert (self->haystacks, t, g_utf8_casefold (hay, -1));
  g_hash_table_insert (self->collate_keys, t,
                       g_utf8_collate_key (t->name ? t->name : "", -1));
}

void
spotifygtk_liked_songs_page_add_track (SpotifyGtkLikedSongsPage *self,
                                       const SpotifyNativeTrack *track)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));
  g_return_if_fail (track != NULL && track->uri != NULL);

  /*
   * Nothing to insert into. An unloaded page fetches the truth when it opens,
   * and seeding it with one row would claim the library holds only that.
   *
   * Keyed on all_tracks rather than `loaded`: `loaded` means "not stale", and
   * the caller invalidates the page right after each of these, so testing it
   * would let the first like through and silently drop every one after it.
   */
  if (!self->all_tracks)
    return;
  if (find_track_index (self, track->uri) >= 0)
    return;

  /* Position 0 is newest: the collection arrives newest-first and SORT_ADDED
   * is that arrival order. See the sort-key enum. */
  SpotifyNativeTrack *copy = spotifygtk_native_track_copy (track);
  g_ptr_array_insert (self->all_tracks, 0, copy);
  index_one_track (self, copy);

  rebuild_sorted (self);
  apply_filter (self);
}

void
spotifygtk_liked_songs_page_remove_track (SpotifyGtkLikedSongsPage *self,
                                          const gchar *uri)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));
  g_return_if_fail (uri != NULL);

  if (!self->all_tracks)   /* see add_track() on why this is not `loaded` */
    return;

  gint at = find_track_index (self, uri);
  if (at < 0)
    return;

  /* Drop the index entries first: they are keyed by the track pointer, which
   * the removal is about to free. */
  SpotifyNativeTrack *t = g_ptr_array_index (self->all_tracks, at);
  if (self->haystacks)    g_hash_table_remove (self->haystacks, t);
  if (self->collate_keys) g_hash_table_remove (self->collate_keys, t);
  g_ptr_array_remove_index (self->all_tracks, at);

  rebuild_sorted (self);
  apply_filter (self);
}

void
spotifygtk_liked_songs_page_refresh (SpotifyGtkLikedSongsPage *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));

  if (self->loaded || self->in_flight)
    return;
  if (self->retry_after && g_get_monotonic_time () < self->retry_after)
    return;

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    spotifygtk_track_list_set_status (self->list, "Not signed in yet.");
    return;
  }

  g_autofree gchar *uri = spotifygtk_native_session_dup_collection_uri (self->session);
  if (!uri) {
    spotifygtk_track_list_set_status (self->list, "Username not resolved yet.");
    return;
  }

  spotifygtk_track_list_set_status (self->list, "Loading…");

  self->in_flight = g_cancellable_new ();

  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, self);

  spotifygtk_native_session_load_tracks (self->session, uri, LIKED_SONGS_LIMIT,
                                         self->in_flight, on_tracks_loaded, ref);
}

void
spotifygtk_liked_songs_page_set_playing_uri (SpotifyGtkLikedSongsPage *self, const gchar *uri, gboolean playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self));
  spotifygtk_track_list_set_playing_uri (self->list, uri, playing);
}

SpotifyGtkTrackList *
spotifygtk_liked_songs_page_get_list (SpotifyGtkLikedSongsPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_LIKED_SONGS_PAGE (self), NULL);
  return self->list;
}

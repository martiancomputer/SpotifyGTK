/*
 * library_page.c — Library page implementation.
 *
 * What is real here: the albums grid is the distinct albums present in the
 * signed-in user's Liked Songs, resolved through the same /context-resolve
 * path everything else uses. Those are genuinely albums the user has saved
 * tracks from, so the grid is real library data, not filler.
 *
 * Playlists used to be a footer here, saying they were unavailable. That cost
 * the grid a permanent row of albums to display one sentence, so it now lives
 * on its own page (see window.c) and this page is albums only.
 */

#include "library_page.h"
#include "album_grid.h"
#include "../spotify/collection.h"

/* Artist names are catalogue metadata rather than image data, so they need a
 * tiny companion map beside the existing cover cache. Values are base64 in
 * the file to preserve arbitrary UTF-8 names without inventing escaping. */
static GHashTable *artist_name_cache;

static gchar *
artist_name_map_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), "spotifygtk",
                           "artist-names", NULL);
}

static void
artist_name_map_load (void)
{
  if (artist_name_cache)
    return;
  artist_name_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, g_free);
  g_autofree gchar *path = artist_name_map_path ();
  g_autofree gchar *data = NULL;
  if (!g_file_get_contents (path, &data, NULL, NULL))
    return;

  g_auto(GStrv) lines = g_strsplit (data, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    gchar *space = strchr (lines[i], ' ');
    if (!space)
      continue;
    *space = '\0';
    gsize len = 0;
    g_autofree guchar *decoded = g_base64_decode (space + 1, &len);
    if (len > 0)
      g_hash_table_replace (artist_name_cache, g_strdup (lines[i]),
                            g_strndup ((const gchar *) decoded, len));
  }
}

static void
artist_name_map_save (void)
{
  if (!artist_name_cache)
    return;
  g_autofree gchar *path = artist_name_map_path ();
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);

  GString *out = g_string_new (NULL);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, artist_name_cache);
  while (g_hash_table_iter_next (&it, &key, &value)) {
    g_autofree gchar *encoded = g_base64_encode ((const guchar *) value,
                                                  strlen (value));
    g_string_append_printf (out, "%s %s\n", (const gchar *) key, encoded);
  }
  g_file_set_contents (path, out->str, (gssize) out->len, NULL);
  g_string_free (out, TRUE);
}

static void
artist_name_cache_store (const gchar *uri, const gchar *name)
{
  if (!uri || !name || !*name)
    return;
  artist_name_map_load ();
  const gchar *known = g_hash_table_lookup (artist_name_cache, uri);
  if (g_strcmp0 (known, name) == 0)
    return;
  g_hash_table_replace (artist_name_cache, g_strdup (uri), g_strdup (name));
  artist_name_map_save ();
}

typedef enum {
  LIBRARY_ALBUMS = 0,
  LIBRARY_EPS,
  LIBRARY_SINGLES,
  LIBRARY_ARTISTS,
  N_LIBRARY_VIEWS
} LibraryView;

struct _SpotifyGtkLibraryPage {
  GtkBox parent_instance;

  SpotifyGtkAlbumGrid  *albums;
  SpotifyGtkAlbumGrid  *artists;
  GtkWidget            *albums_status;   /* empty/error note; loading uses window overlay */
  GtkWidget            *artists_status;
  GtkStack             *content_stack;
  GtkSearchEntry       *filter_entry;
  GtkWidget            *view_buttons[N_LIBRARY_VIEWS];
  LibraryView           active_view;
  GPtrArray            *saved_releases; /* SpotifyNativeRelease*, owned */
  GPtrArray            *saved_album_uris; /* gchar*, gathered across pages */
  GPtrArray            *followed_artist_uris; /* gchar*, copied from window */
  gboolean              artists_populated;
  /* Invalidates callbacks from a previous load. Without it a second load
   * clears and re-splices the model while the first one's reads are still
   * arriving, and a splice destroys the item a bound card is showing -- the
   * failure album_grid.c's add_card comment describes. */
  guint                 generation;
  GtkWidget            *header_revealer; /* title + heading, folds away on scroll */
  SpotifyNativeSession *session;         /* not owned */
  GCancellable         *load_cancel;
  gboolean              loading;
  gboolean              loaded;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkLibraryPage, spotifygtk_library_page, GTK_TYPE_BOX)

enum { LOADING_CHANGED, N_SIGNALS };
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
  g_clear_pointer (&self->saved_album_uris, g_ptr_array_unref);
  g_clear_pointer (&self->saved_releases, g_ptr_array_unref);
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

static gboolean
release_in_view (const SpotifyNativeRelease *release, LibraryView view)
{
  switch (view) {
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

static void
show_release_view (SpotifyGtkLibraryPage *self)
{
  if (!self->saved_releases || self->active_view == LIBRARY_ARTISTS)
    return;

  guint count = 0;
  for (guint i = 0; i < self->saved_releases->len; i++)
    if (release_in_view (g_ptr_array_index (self->saved_releases, i),
                         self->active_view))
      count++;

  g_autofree SpotifyGtkCardSpec *specs = g_new0 (SpotifyGtkCardSpec,
                                                 MAX (count, 1));
  g_autoptr(GPtrArray) subs = g_ptr_array_new_with_free_func (g_free);
  guint out = 0;
  for (guint i = 0; i < self->saved_releases->len; i++) {
    const SpotifyNativeRelease *release = g_ptr_array_index (self->saved_releases, i);
    if (!release_in_view (release, self->active_view))
      continue;
    gchar *sub = release->year > 0 ? g_strdup_printf ("%d", release->year)
                                   : g_strdup ("");
    g_ptr_array_add (subs, sub);
    specs[out].uri      = release->uri;
    specs[out].title    = release->name ? release->name : "Unknown release";
    specs[out].subtitle = sub;
    specs[out].cover_id = release->cover_id;
    out++;
  }
  spotifygtk_album_grid_set_cards (self->albums, specs, out);

  const gchar *empty = self->active_view == LIBRARY_EPS
    ? "No saved EPs yet."
    : self->active_view == LIBRARY_SINGLES
      ? "No saved singles yet."
      : "No saved albums yet.";
  gtk_widget_set_visible (self->albums_status, out == 0);
  gtk_label_set_text (GTK_LABEL (self->albums_status), out == 0 ? empty : "");
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

  if (!self)
    return;
  if (cl_generation != self->generation)
    return;   /* a newer load has already replaced this one */
  g_clear_object (&self->load_cancel);
  set_loading (self, FALSE);
  if (!albums) {
    if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      gtk_label_set_text (GTK_LABEL (self->albums_status),
                          "Could not load your albums right now.");
    return;
  }
  self->loaded = TRUE;

  g_clear_pointer (&self->saved_releases, g_ptr_array_unref);
  self->saved_releases = g_steal_pointer (&albums);
  show_release_view (self);
}

/* Every album URI in the collection set. Liked tracks share that set and are
 * skipped here by prefix. */
static void
on_saved_page (gboolean ok, guint16 status, SpotifyCollectionItem *items,
               guint n_items, const gchar *next_token, gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;

  /* Mercury replies cannot be cancelled at the transport layer. A session
   * replacement cancels and clears this marker, so discard its late pages
   * instead of continuing the old read against the new session. */
  if (!self->load_cancel || g_cancellable_is_cancelled (self->load_cancel))
    return;

  guint mine = self->generation;

  if (!ok) {
    g_clear_object (&self->load_cancel);
    set_loading (self, FALSE);
    gtk_label_set_text (GTK_LABEL (self->albums_status),
                        "Could not read your library right now.");
    g_message ("library: saved-album read failed (status %u)", status);
    return;
  }

  if (!self->saved_album_uris)
    self->saved_album_uris = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < n_items; i++) {
    if (!items[i].uri || items[i].is_removed)
      continue;
    if (g_str_has_prefix (items[i].uri, "spotify:album:"))
      g_ptr_array_add (self->saved_album_uris, g_strdup (items[i].uri));
  }

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  g_autofree gchar *user = spotifygtk_native_session_dup_username (self->session);

  if (next_token && *next_token && m && user) {
    spotifygtk_collection_v2_read_page (m, user, SPOTIFYGTK_COLLECTION_SET_LIKED,
                                        next_token, 500, on_saved_page, self);
    return;
  }

  LibLoad *cl = g_new0 (LibLoad, 1);
  g_weak_ref_init (&cl->page, self);
  cl->generation = mine;
  spotifygtk_native_session_load_albums (self->session,
    (const gchar *const *) self->saved_album_uris->pdata,
    self->saved_album_uris->len, self->load_cancel, on_album_meta_loaded, cl);
}

void
spotifygtk_library_page_set_session (SpotifyGtkLibraryPage *self,
                                     SpotifyNativeSession  *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));

  self->session = session;
  self->generation++;

  if (self->load_cancel) {
    g_cancellable_cancel (self->load_cancel);
    g_clear_object (&self->load_cancel);
  }
  set_loading (self, FALSE);
  self->loaded = FALSE;
  g_clear_pointer (&self->saved_releases, g_ptr_array_unref);
}

void
spotifygtk_library_page_refresh (SpotifyGtkLibraryPage *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));

  /* Resolving every saved album is substantial on a large collection. Do it
   * on first visit instead of behind Home during every cold start. */
  if (self->loaded || self->load_cancel)
    return;
  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY)
    return;

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  g_autofree gchar *user = spotifygtk_native_session_dup_username (self->session);
  if (!m || !user)
    return;

  gtk_label_set_text (GTK_LABEL (self->albums_status), "");
  gtk_widget_set_visible (self->albums_status, FALSE);
  set_loading (self, TRUE);

  self->load_cancel = g_cancellable_new ();
  self->generation++;
  g_clear_pointer (&self->saved_album_uris, g_ptr_array_unref);
  spotifygtk_collection_v2_read_page (m, user, SPOTIFYGTK_COLLECTION_SET_LIKED,
                                      NULL, 500, on_saved_page, self);
}

SpotifyGtkAlbumGrid *
spotifygtk_library_page_get_album_grid (SpotifyGtkLibraryPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self), NULL);
  return self->albums;
}

SpotifyGtkAlbumGrid *
spotifygtk_library_page_get_artist_grid (SpotifyGtkLibraryPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self), NULL);
  return self->artists;
}

static void
populate_artist_cards (SpotifyGtkLibraryPage *self)
{
  if (self->artists_populated)
    return;
  self->artists_populated = TRUE;

  guint n = self->followed_artist_uris ? self->followed_artist_uris->len : 0;
  artist_name_map_load ();
  g_autofree SpotifyGtkCardSpec *specs = g_new0 (SpotifyGtkCardSpec, MAX (n, 1));
  for (guint i = 0; i < n; i++) {
    const gchar *uri = g_ptr_array_index (self->followed_artist_uris, i);
    const gchar *known_name = g_hash_table_lookup (artist_name_cache, uri);
    specs[i].uri = uri;
    specs[i].title = known_name ? known_name : "Artist";
    specs[i].subtitle = "Followed artist";
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
on_artist_card_image (const gchar *cover_id, gpointer user_data)
{
  ArtistCardLoad *load = user_data;
  load->cover_id = g_strdup (cover_id);
  artist_card_load_complete (load);
}

static void
on_artist_card_tracks (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ArtistCardLoad *load = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) tracks = spotifygtk_native_session_load_tracks_finish (
    SPOTIFYGTK_NATIVE_SESSION (source), result, &error);
  if (tracks && tracks->len > 0) {
    const SpotifyNativeTrack *track = g_ptr_array_index (tracks, 0);
    if (track && track->artists && *track->artists) {
      const gchar *comma = strstr (track->artists, ", ");
      load->name = comma ? g_strndup (track->artists, comma - track->artists)
                         : g_strdup (track->artists);
      artist_name_cache_store (load->uri, load->name);
    }
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
  artist_name_map_load ();
  const gchar *known_name = g_hash_table_lookup (artist_name_cache, uri);
  if (known_name)
    load->name = g_strdup (known_name);
  load->remaining = known_name ? 1 : 2;
  spotifygtk_native_session_get_artist_portrait (self->session, uri,
                                                  on_artist_card_image, load);
  if (!known_name)
    spotifygtk_native_session_load_tracks (self->session, uri, 1, NULL,
                                           on_artist_card_tracks, load);
  (void) grid;
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
  self->active_view = view;

  /* GtkStack keeps the hidden child alive, including each card's texture.
   * Switching views must relinquish those widget references; the compressed
   * disk cache is deliberately retained and makes restoration inexpensive. */
  if (artists)
    spotifygtk_album_grid_release_covers (self->albums);
  else
    spotifygtk_album_grid_release_covers (self->artists);
  gtk_stack_set_visible_child_name (self->content_stack,
                                    artists ? "artists" : "releases");
  if (artists)
    populate_artist_cards (self);
  else
    show_release_view (self);
  spotifygtk_album_grid_reload_covers (artists ? self->artists : self->albums);
}

static void
on_filter_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SpotifyGtkLibraryPage *self = user_data;
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  spotifygtk_album_grid_set_filter_text (self->albums, text);
  spotifygtk_album_grid_set_filter_text (self->artists, text);
}

void
spotifygtk_library_page_set_covers_loaded (SpotifyGtkLibraryPage *self,
                                           gboolean               loaded)
{
  g_return_if_fail (SPOTIFYGTK_IS_LIBRARY_PAGE (self));
  if (!loaded) {
    spotifygtk_album_grid_release_covers (self->albums);
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
  gdouble value = gtk_adjustment_get_value (adj);
  gboolean revealed =
    gtk_revealer_get_reveal_child (GTK_REVEALER (self->header_revealer));

  if (revealed && value > HEADER_HIDE_AT)
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_revealer), FALSE);
  else if (!revealed && value < HEADER_SHOW_AT)
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_revealer), TRUE);
}


/* === Building blocks === */

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

  GtkWidget *title = gtk_label_new ("Library");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (header), title);

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
  const gchar *view_names[] = { "Albums", "EPs", "Singles", "Artists" };
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
  /* Match the small breathing room between controls and results on Liked
   * Songs; without it the first card row touches the loading-rule edge. */
  gtk_widget_set_margin_top (GTK_WIDGET (self->content_stack), 4);
  gtk_widget_set_vexpand (GTK_WIDGET (self->content_stack), TRUE);

  GtkWidget *albums_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  self->albums_status = gtk_label_new ("Not signed in yet.");
  gtk_widget_add_css_class (self->albums_status, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (self->albums_status), 0.0);
  gtk_widget_set_margin_start (self->albums_status, 35);
  gtk_box_append (GTK_BOX (albums_page), self->albums_status);

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
  gtk_widget_set_vexpand (GTK_WIDGET (self->albums), TRUE);
  gtk_box_append (GTK_BOX (albums_page), GTK_WIDGET (self->albums));
  gtk_stack_add_named (self->content_stack, albums_page, "releases");

  GtkWidget *artists_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  self->artists_status = gtk_label_new ("");
  gtk_widget_add_css_class (self->artists_status, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (self->artists_status), 0.0);
  gtk_widget_set_margin_start (self->artists_status, 35);
  gtk_widget_set_visible (self->artists_status, FALSE);
  gtk_box_append (GTK_BOX (artists_page), self->artists_status);
  self->artists = spotifygtk_album_grid_new_grid ();
  spotifygtk_album_grid_set_content_margins (self->artists, 35, 22);
  gtk_widget_set_vexpand (GTK_WIDGET (self->artists), TRUE);
  g_signal_connect (self->artists, "card-needs-resolve",
                    G_CALLBACK (on_artist_card_needs_resolve), self);
  gtk_box_append (GTK_BOX (artists_page), GTK_WIDGET (self->artists));
  gtk_stack_add_named (self->content_stack, artists_page, "artists");
  gtk_stack_set_visible_child_name (self->content_stack, "releases");
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->content_stack));
  self->active_view = LIBRARY_ALBUMS;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->view_buttons[0]), TRUE);

  GtkAdjustment *vadj = spotifygtk_album_grid_get_vadjustment (self->albums);
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

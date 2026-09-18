/*
 * search_page.c — Catalog search page implementation.
 *
 * Typing debounces for SEARCH_DEBOUNCE_MS before hitting the network, so a
 * fast typist produces one query rather than one per keystroke. Each
 * dispatched query carries a serial and its own GCancellable: the serial
 * drops results that a newer query has superseded, and the cancellable
 * stops the older request rather than merely ignoring its answer.
 */

#include "search_page.h"
#include "track_list.h"
#include "album_grid.h"
#include "settings.h"
#include "smooth_scroll.h"
#include "../log_file.h"

#include "spotify/spclient.h"   /* build_search_uri */

#define SEARCH_DEBOUNCE_MS 350
#define SEARCH_RESULT_LIMIT SPOTIFYGTK_SESSION_MAX_TRACKS

struct _SpotifyGtkSearchPage {
  GtkBox parent_instance;

  GtkSearchEntry      *entry;
  SpotifyGtkTrackList *results;
  SpotifyGtkAlbumGrid *albums;
  GtkWidget           *albums_section;   /* Album shelf; hidden when empty */
  SpotifyGtkAlbumGrid *playlists;
  GtkWidget           *playlists_section;
  GCancellable        *playlist_request;
  JsonNode            *playlist_answer;
  gchar               *result_query;
  gboolean             aggressive;
  gboolean             compact;
  gboolean             have_albums;
  gboolean             have_playlists;

  SpotifyNativeSession *session;

  GCancellable *in_flight;
  guint         debounce_id;
  gboolean      searching;
  guint64       serial;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkSearchPage, spotifygtk_search_page, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, LOADING_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];
static void request_playlists (SpotifyGtkSearchPage *self, const gchar *query);

static void
sync_result_layout (SpotifyGtkSearchPage *self)
{
  gtk_widget_set_visible (self->albums_section, !self->compact && self->have_albums);
  gtk_widget_set_visible (self->playlists_section, !self->compact && self->have_playlists);
  if (self->compact) {
    g_autoptr(GPtrArray) contexts = spotifygtk_album_grid_snapshot_contexts (self->albums);
    g_autoptr(GPtrArray) playlists = spotifygtk_album_grid_snapshot_contexts (self->playlists);
    g_ptr_array_extend_and_steal (contexts, g_steal_pointer (&playlists));
    spotifygtk_track_list_set_search_contexts (self->results, contexts);
  } else {
    spotifygtk_track_list_set_search_contexts (self->results, NULL);
  }
}

typedef struct {
  GWeakRef page;
  guint64  serial;
  gchar   *query;
} SearchClosure;

/* Case, spaces and punctuation should not prevent an artist query such as
 * "badcomputer" from matching the catalog spelling "Bad Computer". */
static gchar *
search_key (const gchar *text)
{
  g_autofree gchar *folded = g_utf8_casefold (text ? text : "", -1);
  GString *key = g_string_sized_new (strlen (folded));
  for (const gchar *p = folded; *p;) {
    gunichar ch = g_utf8_get_char (p);
    if (g_unichar_isalnum (ch))
      g_string_append_unichar (key, ch);
    p = g_utf8_next_char (p);
  }
  return g_string_free (key, FALSE);
}

static guint
search_rank (const SpotifyNativeTrack *track, const gchar *query_key)
{
  g_autofree gchar *artist = search_key (track ? track->artists : NULL);
  g_autofree gchar *name = search_key (track ? track->name : NULL);
  g_autofree gchar *album = search_key (track ? track->album : NULL);

  if (*query_key && g_strcmp0 (artist, query_key) == 0) return 0;
  if (*query_key && strstr (artist, query_key))         return 1;
  if (*query_key && g_strcmp0 (name, query_key) == 0)  return 2;
  if (*query_key && strstr (name, query_key))           return 3;
  if (*query_key && g_strcmp0 (album, query_key) == 0) return 4;
  if (*query_key && strstr (album, query_key))          return 5;
  return 6;
}

static gint
compare_search_tracks (gconstpointer a, gconstpointer b, gpointer user_data)
{
  const SpotifyNativeTrack *ta = *(SpotifyNativeTrack * const *) a;
  const SpotifyNativeTrack *tb = *(SpotifyNativeTrack * const *) b;
  const gchar *query_key = user_data;
  guint ra = search_rank (ta, query_key);
  guint rb = search_rank (tb, query_key);
  return (ra > rb) - (ra < rb);
}

static JsonObject *
object_child (JsonObject *object, const gchar *key)
{
  JsonNode *node = object ? json_object_get_member (object, key) : NULL;
  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

static JsonArray *
array_child (JsonObject *object, const gchar *key)
{
  JsonNode *node = object ? json_object_get_member (object, key) : NULL;
  return node && JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
}

static const gchar *
string_child (JsonObject *object, const gchar *key)
{
  JsonNode *node = object ? json_object_get_member (object, key) : NULL;
  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
    ? json_node_get_string (node) : NULL;
}

static void
render_playlists (SpotifyGtkSearchPage *self)
{
  JsonObject *root = self->playlist_answer && JSON_NODE_HOLDS_OBJECT (self->playlist_answer)
    ? json_node_get_object (self->playlist_answer) : NULL;
  JsonObject *search = object_child (object_child (root, "data"), "searchV2");
  JsonArray *items = array_child (object_child (search, "playlists"), "items");
  if (!items) items = array_child (object_child (search, "playlistsV2"), "items");
  guint n = items ? MIN (20u, json_array_get_length (items)) : 0;
  g_autofree SpotifyGtkCardSpec *cards = g_new0 (SpotifyGtkCardSpec, n);
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  g_autofree gchar *key = search_key (self->result_query);
  guint shown = 0;
  for (guint i = 0; i < n; i++) {
    JsonNode *node = json_array_get_element (items, i);
    if (!JSON_NODE_HOLDS_OBJECT (node)) continue;
    JsonObject *item = json_node_get_object (node);
    JsonObject *data = object_child (object_child (item, "item"), "data");
    if (!data) data = object_child (item, "data");
    if (!data) data = item;
    const gchar *uri = string_child (data, "uri");
    const gchar *name = string_child (data, "name");
    if (!uri || !name || !g_str_has_prefix (uri, "spotify:playlist:") ||
        g_hash_table_contains (seen, uri)) continue;
    g_hash_table_add (seen, (gpointer) uri);
    JsonObject *owner = object_child (object_child (data, "ownerV2"), "data");
    const gchar *owner_name = string_child (owner, "name");
    if (!owner_name) owner_name = string_child (owner, "username");
    if (self->aggressive) {
      g_autofree gchar *name_key = search_key (name);
      g_autofree gchar *owner_key = search_key (owner_name);
      if (!*key || (!strstr (name_key, key) && !strstr (owner_key, key))) continue;
    }
    gchar *id = NULL;
    JsonArray *images = array_child (object_child (data, "images"), "items");
    for (guint j = 0; images && j < json_array_get_length (images) && !id; j++) {
      JsonNode *image = json_array_get_element (images, j);
      if (!JSON_NODE_HOLDS_OBJECT (image)) continue;
      JsonArray *sources = array_child (json_node_get_object (image), "sources");
      for (guint k = 0; sources && k < json_array_get_length (sources); k++) {
        JsonNode *source = json_array_get_element (sources, k);
        const gchar *url = JSON_NODE_HOLDS_OBJECT (source)
          ? string_child (json_node_get_object (source), "url") : NULL;
        const gchar *prefix = "https://i.scdn.co/image/";
        if (url && g_str_has_prefix (url, prefix)) {
          id = g_strdup (url + strlen (prefix));
          break;
        }
      }
    }
    if (id) g_ptr_array_add (ids, id);
    cards[shown++] = (SpotifyGtkCardSpec) { uri, name, owner_name, id };
  }
  spotifygtk_album_grid_set_cards (self->playlists, cards, shown);
  self->have_playlists = shown > 0;
  sync_result_layout (self);
}

static void
on_search_layout_changed (SpotifyGtkSettings *settings, gpointer data)
{
  SpotifyGtkSearchPage *self = data;
  gboolean compact = spotifygtk_settings_get_compact_mode (settings);
  if (self->compact != compact) {
    self->compact = compact;
    sync_result_layout (self);
  }
  gboolean aggressive = spotifygtk_settings_get_aggressive_filtering (settings);
  if (self->aggressive != aggressive) {
    self->aggressive = aggressive;
    render_playlists (self);
  }
}

#ifdef SPOTIFYGTK_UI_TESTS
/* Feed an offline Pathfinder fixture into the same renderer used by the
 * real cancellable response. No production API or persisted state is added. */
void
spotifygtk_search_page_test_playlists (SpotifyGtkSearchPage *self,
                                      const gchar *json, const gchar *query)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, json, -1, NULL));
  g_clear_pointer (&self->playlist_answer, json_node_unref);
  self->playlist_answer = json_node_copy (json_parser_get_root (parser));
  g_free (self->result_query);
  self->result_query = g_strdup (query);
  render_playlists (self);
}
#endif

static void
on_playlists_loaded (GObject *source, GAsyncResult *result, gpointer data)
{
  SearchClosure *cl = data;
  g_autoptr(SpotifyGtkSearchPage) self = g_weak_ref_get (&cl->page);
  guint64 serial = cl->serial;
  g_weak_ref_clear (&cl->page);
  g_free (cl->query);
  g_free (cl);
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonNode) answer = spotifygtk_native_session_search_playlists_finish (
    SPOTIFYGTK_NATIVE_SESSION (source), result, &error);
  if (!self || serial != self->serial) return;
  g_clear_object (&self->playlist_request);
  if (!answer) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_message ("search: optional playlist query unavailable: %s", error ? error->message : "empty response");
    return;
  }
  g_clear_pointer (&self->playlist_answer, json_node_unref);
  self->playlist_answer = g_steal_pointer (&answer);
  render_playlists (self);
}

static void
request_playlists (SpotifyGtkSearchPage *self, const gchar *query)
{
  g_free (self->result_query);
  self->result_query = g_strdup (query);
  if (!self->session) return;
  self->playlist_request = g_cancellable_new ();
  SearchClosure *cl = g_new0 (SearchClosure, 1);
  g_weak_ref_init (&cl->page, self);
  cl->serial = self->serial;
  spotifygtk_native_session_search_playlists (self->session, query,
    self->playlist_request, on_playlists_loaded, cl);
}

static void
set_searching (SpotifyGtkSearchPage *self, gboolean searching)
{
  searching = !!searching;
  if (self->searching == searching)
    return;

  self->searching = searching;
  g_signal_emit (self, signals[LOADING_CHANGED], 0, searching);
}

static void
on_track_activated (SpotifyGtkTrackList *list, gpointer track, gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  g_signal_emit (self, signals[TRACK_ACTIVATED], 0, track);
  (void) list;
}

static void
on_context_activated (SpotifyGtkTrackList *list, const SpotifyNativeTrack *context,
                      SpotifyGtkSearchPage *self)
{
  SpotifyGtkAlbumGrid *grid = g_str_has_prefix (context->uri, "spotify:playlist:")
    ? self->playlists : self->albums;
  g_signal_emit_by_name (grid, "album-activated", context->uri, context->name);
  (void) list;
}

static void
on_context_menu (SpotifyGtkTrackList *list, const SpotifyNativeTrack *context,
                 GtkWidget *anchor, gdouble x, gdouble y, SpotifyGtkSearchPage *self)
{
  SpotifyGtkAlbumGrid *grid = g_str_has_prefix (context->uri, "spotify:playlist:")
    ? self->playlists : self->albums;
  SpotifyGtkCardSpec spec = { context->uri, context->name, context->artists, context->cover_id };
  spotifygtk_album_grid_present_context_menu (grid, anchor, &spec, x, y);
  (void) list;
}

static void
set_albums_visible (SpotifyGtkSearchPage *self, gboolean visible)
{
  self->have_albums = visible;
  spotifygtk_track_list_set_top_inset (self->results, 0);
  if (!visible)
    spotifygtk_album_grid_clear (self->albums);
  sync_result_layout (self);
}

static void
on_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  SearchClosure        *cl      = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(SpotifyGtkSearchPage) self = g_weak_ref_get (&cl->page);
  guint64 serial = cl->serial;
  g_autofree gchar *query = g_steal_pointer (&cl->query);
  g_weak_ref_clear (&cl->page);
  g_free (cl);

  g_autoptr(GPtrArray) tracks =
    spotifygtk_native_session_load_tracks_finish (session, result, &err);
  spotifygtk_runtime_schedule_heap_trim ();

  if (!self)
    return;                       /* page went away mid-request */
  if (serial != self->serial)
    return;                       /* superseded by a newer query */

  g_clear_object (&self->in_flight);
  set_searching (self, FALSE);

  if (!tracks) {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;
    g_autofree gchar *msg = g_strdup_printf ("Search failed: %s", err->message);
    set_albums_visible (self, FALSE);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_set_status (self->results, msg);
    return;
  }

  request_playlists (self, query);

  if (tracks->len == 0) {
    set_albums_visible (self, FALSE);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_set_status (self->results, "No results.");
    return;
  }

  /* These are already ranked catalog results from the desktop search query.
   * The old 20-track context needed a word-match filter because it contained
   * playback filler; applying that filter here would throw away Spotify's
   * fuzzy/semantic matches and make the expanded result set look small again. */
  g_autoptr(GPtrArray) shown = g_ptr_array_ref (tracks);
  if (spotifygtk_settings_get_aggressive_filtering (
        spotifygtk_settings_get_default ())) {
    g_autofree gchar *query_key = search_key (query);
    g_ptr_array_sort_with_data (shown, compare_search_tracks, query_key);
  }

  /* The albums shelf is the distinct albums present in these very results --
   * real matches, grouped, not a second query. */
  guint n_albums = spotifygtk_album_grid_set_from_tracks (self->albums, shown, 40);
  spotifygtk_track_list_set_native_tracks (self->results, shown);
  set_albums_visible (self, n_albums > 0);
}

#ifdef SPOTIFYGTK_UI_TESTS
void
spotifygtk_search_page_test_complete (SpotifyGtkSearchPage *self,
                                     GPtrArray *tracks, const gchar *query)
{
  g_autoptr(SpotifyNativeSession) session = spotifygtk_native_session_new ();
  g_autoptr(GTask) task = g_task_new (session, NULL, NULL, NULL);
  g_task_return_pointer (task, g_ptr_array_ref (tracks), (GDestroyNotify) g_ptr_array_unref);
  SearchClosure *cl = g_new0 (SearchClosure, 1);
  g_weak_ref_init (&cl->page, self);
  cl->serial = self->serial;
  cl->query = g_strdup (query);
  on_tracks_loaded (G_OBJECT (session), G_ASYNC_RESULT (task), cl);
}
#endif

static gboolean
dispatch_search (gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  self->debounce_id = 0;

  const gchar *query = gtk_editable_get_text (GTK_EDITABLE (self->entry));

  /* Any previous query is now obsolete: bump the serial so a late answer is
   * discarded, and cancel it so it stops occupying the session. */
  self->serial++;
  if (self->in_flight) {
    g_cancellable_cancel (self->in_flight);
    g_clear_object (&self->in_flight);
  }
  if (self->playlist_request) g_cancellable_cancel (self->playlist_request);
  g_clear_object (&self->playlist_request);
  g_clear_pointer (&self->playlist_answer, json_node_unref);
  spotifygtk_album_grid_clear (self->playlists);
  self->have_playlists = FALSE;
  sync_result_layout (self);

  if (!query || !*query) {
    set_searching (self, FALSE);
    set_albums_visible (self, FALSE);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_set_status (self->results, NULL);
    return G_SOURCE_REMOVE;
  }

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    set_searching (self, FALSE);
    spotifygtk_track_list_set_status (self->results, "Not signed in yet.");
    return G_SOURCE_REMOVE;
  }

  g_autofree gchar *uri = spotifygtk_spclient_build_search_uri (query);
  if (!uri) {
    set_searching (self, FALSE);
    return G_SOURCE_REMOVE;
  }

  /* Keep any previous results in place while their replacement arrives. The
   * progress strip floats above the page, so searching neither inserts a row
   * nor makes the content jump between list and status views. */
  spotifygtk_track_list_set_status (self->results, NULL);
  set_searching (self, TRUE);

  self->in_flight = g_cancellable_new ();

  SearchClosure *cl = g_new0 (SearchClosure, 1);
  g_weak_ref_init (&cl->page, self);
  cl->serial = self->serial;
  cl->query = g_strdup (query);

  spotifygtk_native_session_load_tracks (self->session, uri, SEARCH_RESULT_LIMIT,
                                         self->in_flight, on_tracks_loaded, cl);
  return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  self->serial++;
  if (self->in_flight) g_cancellable_cancel (self->in_flight);
  if (self->playlist_request) g_cancellable_cancel (self->playlist_request);
  g_clear_handle_id (&self->debounce_id, g_source_remove);
  self->debounce_id = g_timeout_add (SEARCH_DEBOUNCE_MS, dispatch_search, self);
  (void) entry;
}

static void
on_search_activate (GtkSearchEntry *entry, gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  g_clear_handle_id (&self->debounce_id, g_source_remove);
  dispatch_search (self);
  (void) entry;
}

static void
spotifygtk_search_page_dispose (GObject *object)
{
  SpotifyGtkSearchPage *self = SPOTIFYGTK_SEARCH_PAGE (object);

  g_clear_handle_id (&self->debounce_id, g_source_remove);
  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  if (self->playlist_request) g_cancellable_cancel (self->playlist_request);
  g_clear_object (&self->playlist_request);
  g_clear_pointer (&self->playlist_answer, json_node_unref);
  g_clear_pointer (&self->result_query, g_free);
  g_clear_object (&self->session);

  G_OBJECT_CLASS (spotifygtk_search_page_parent_class)->dispose (object);
}

static void
spotifygtk_search_page_class_init (SpotifyGtkSearchPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_search_page_dispose;

  signals[TRACK_ACTIVATED] = g_signal_new ("track-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[LOADING_CHANGED] = g_signal_new ("loading-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
spotifygtk_search_page_init (SpotifyGtkSearchPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* Header and results are ordinary siblings in one scrollable page. No
   * overlay/inset: scrolling cannot leave the header floating over cards. */
  GtkWidget *base = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);
  gtk_widget_set_vexpand (scroller, TRUE);
  spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (scroller), GTK_ORIENTATION_VERTICAL);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top (header, 24);
  gtk_widget_set_margin_bottom (header, 14);
  GtkWidget *title = gtk_label_new ("Search");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.5);
  gtk_box_append (GTK_BOX (header), title);
  self->entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->entry), 460, -1);
  gtk_widget_set_halign (GTK_WIDGET (self->entry), GTK_ALIGN_CENTER);
  gtk_search_entry_set_placeholder_text (self->entry, "Songs, artists, albums, playlists");
  g_signal_connect (self->entry, "search-changed", G_CALLBACK (on_search_changed), self);
  g_signal_connect (self->entry, "activate", G_CALLBACK (on_search_activate), self);
  gtk_box_append (GTK_BOX (header), GTK_WIDGET (self->entry));
  gtk_box_append (GTK_BOX (base), header);

  /* Expanded shelves surround the song list. Compact contexts share its
   * vertically recycled rows instead of creating horizontal shelves. */
  self->albums_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  /* A horizontal shelf must clip at the pane edge. An outer end margin first
   * clips the next card and then leaves a page-coloured strip beside it—the
   * same light-theme rectangle Home used to show. */
  gtk_widget_set_margin_end (self->albums_section, 0);
  gtk_widget_set_visible (self->albums_section, FALSE);

  self->albums = spotifygtk_album_grid_new_shelf ();
  spotifygtk_album_grid_set_show_types (self->albums, TRUE);
  spotifygtk_album_grid_set_external_viewport (self->albums, GTK_SCROLLED_WINDOW (scroller));
  gtk_box_append (GTK_BOX (self->albums_section), GTK_WIDGET (self->albums));
  gtk_box_append (GTK_BOX (base), self->albums_section);

  self->results = spotifygtk_track_list_new ();
  spotifygtk_track_list_set_show_type (self->results, TRUE);
  spotifygtk_track_list_set_inline (self->results, TRUE);
  spotifygtk_track_list_set_external_viewport (self->results, GTK_SCROLLED_WINDOW (scroller));
  spotifygtk_track_list_set_content_margins (self->results, 35, 14);
  /* No bottom margin; see the note in liked_songs_page.c. */
  g_signal_connect (self->results, "track-activated", G_CALLBACK (on_track_activated), self);
  g_signal_connect (self->results, "context-activated", G_CALLBACK (on_context_activated), self);
  g_signal_connect (self->results, "context-menu", G_CALLBACK (on_context_menu), self);
  gtk_box_append (GTK_BOX (base), GTK_WIDGET (self->results));

  self->playlists_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *playlist_heading = gtk_label_new ("Playlists");
  gtk_label_set_xalign (GTK_LABEL (playlist_heading), 0);
  gtk_widget_set_margin_start (playlist_heading, 35);
  gtk_widget_set_margin_top (playlist_heading, 16);
  gtk_widget_add_css_class (playlist_heading, "section-heading");
  gtk_box_append (GTK_BOX (self->playlists_section), playlist_heading);
  self->playlists = spotifygtk_album_grid_new_shelf ();
  spotifygtk_album_grid_set_show_types (self->playlists, TRUE);
  spotifygtk_album_grid_set_external_viewport (self->playlists, GTK_SCROLLED_WINDOW (scroller));
  gtk_box_append (GTK_BOX (self->playlists_section), GTK_WIDGET (self->playlists));
  gtk_box_append (GTK_BOX (base), self->playlists_section);
  gtk_widget_set_visible (self->playlists_section, FALSE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), base);
  gtk_box_append (GTK_BOX (self), scroller);
  self->aggressive = spotifygtk_settings_get_aggressive_filtering (spotifygtk_settings_get_default ());
  on_search_layout_changed (spotifygtk_settings_get_default (), self);
  g_signal_connect_object (spotifygtk_settings_get_default (), "changed",
    G_CALLBACK (on_search_layout_changed), self, 0);
}

SpotifyGtkSearchPage *
spotifygtk_search_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_SEARCH_PAGE, NULL);
}

void
spotifygtk_search_page_set_session (SpotifyGtkSearchPage *self, SpotifyNativeSession *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self));

  self->serial++;
  if (self->in_flight) g_cancellable_cancel (self->in_flight);
  if (self->playlist_request) g_cancellable_cancel (self->playlist_request);
  g_clear_object (&self->in_flight);
  g_clear_object (&self->playlist_request);
  g_clear_object (&self->session);
  self->session = session ? g_object_ref (session) : NULL;
}

void
spotifygtk_search_page_set_playing_uri (SpotifyGtkSearchPage *self, const gchar *uri, gboolean playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self));
  spotifygtk_track_list_set_playing_uri (self->results, uri, playing);
}

SpotifyGtkTrackList *
spotifygtk_search_page_get_list (SpotifyGtkSearchPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self), NULL);
  return self->results;
}

SpotifyGtkAlbumGrid *
spotifygtk_search_page_get_album_grid (SpotifyGtkSearchPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self), NULL);
  return self->albums;
}

SpotifyGtkAlbumGrid *
spotifygtk_search_page_get_playlist_grid (SpotifyGtkSearchPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SEARCH_PAGE (self), NULL);
  return self->playlists;
}

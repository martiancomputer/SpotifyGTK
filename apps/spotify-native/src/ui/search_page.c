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
#include "smooth_scroll.h"
#include "settings.h"
#include "../log_file.h"

#include "spotify/spclient.h"   /* build_search_uri */

#define SEARCH_DEBOUNCE_MS 350
#define SEARCH_RESULT_LIMIT SPOTIFYGTK_SESSION_MAX_TRACKS

#define SEARCH_ALBUM_LIMIT 50
#define SEARCH_PLAYLIST_LIMIT 50

struct _SpotifyGtkSearchPage {
  GtkBox parent_instance;

  GtkSearchEntry      *entry;
  GtkWidget           *page_header;    /* borrowed: owned by TrackList */
  GtkWidget           *scroll_overlay; /* borrowed: owned by this page */
  GtkWidget           *scrollbar;      /* borrowed: owned by overlay */
  gdouble              header_extent;
  gint                 scrollbar_top;
  SpotifyGtkTrackList *results;
  SpotifyGtkAlbumGrid *albums;
  GtkWidget           *albums_section;
  GtkLabel            *message;
  GPtrArray           *contexts; /* owned album and playlist display records */
  guint                album_count;
  guint                track_count;
  JsonNode            *playlist_answer;
  gchar               *result_query;
  gboolean             compact;
  gboolean             aggressive;

  SpotifyNativeSession *session;

  GCancellable *in_flight;
  GCancellable *playlist_request;
  guint         debounce_id;
  gboolean      searching;
  guint64       serial;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkSearchPage, spotifygtk_search_page, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, LOADING_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

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
object_child (JsonObject *parent, const gchar *name)
{
  JsonNode *node = parent ? json_object_get_member (parent, name) : NULL;
  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

static JsonArray *
array_child (JsonObject *parent, const gchar *name)
{
  JsonNode *node = parent ? json_object_get_member (parent, name) : NULL;
  return node && JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
}

static const gchar *
string_child (JsonObject *parent, const gchar *name)
{
  JsonNode *node = parent ? json_object_get_member (parent, name) : NULL;
  return node && JSON_NODE_HOLDS_VALUE (node) &&
    json_node_get_value_type (node) == G_TYPE_STRING
    ? json_node_get_string (node) : NULL;
}

static void
sync_result_layout (SpotifyGtkSearchPage *self)
{
  gtk_widget_set_visible (self->albums_section,
    !self->compact && self->contexts->len > 0);
  spotifygtk_track_list_set_search_contexts (self->results,
    self->compact ? self->contexts : NULL);
}

static void
show_message (SpotifyGtkSearchPage *self, const gchar *text)
{
  gtk_label_set_text (self->message, text ? text : "");
  gtk_widget_set_visible (GTK_WIDGET (self->message), text && *text);
}

static gint
search_scrollbar_top (SpotifyGtkSearchPage *self, GtkAdjustment *adj);

static void
update_scrollbar_visibility (SpotifyGtkSearchPage *self)
{
  if (!self->scroll_overlay || !self->scrollbar)
    return;
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (
    spotifygtk_track_list_get_scroller (self->results));
  gint height = gtk_widget_get_height (self->scroll_overlay);
  gboolean scrollable = self->header_extent > 0 &&
    gtk_adjustment_get_upper (adj) > gtk_adjustment_get_page_size (adj) + 1 &&
    self->header_extent - gtk_adjustment_get_value (adj) < height;
  gboolean changed = gtk_widget_get_visible (self->scrollbar) != scrollable;
  if (changed)
    gtk_widget_set_visible (self->scrollbar, scrollable);
  if (scrollable && (changed ||
      search_scrollbar_top (self, adj) != self->scrollbar_top))
    gtk_widget_queue_allocate (self->scroll_overlay);
}

static gint
search_scrollbar_top (SpotifyGtkSearchPage *self, GtkAdjustment *adj)
{
  gint height = gtk_widget_get_height (self->scroll_overlay);
  return CLAMP ((gint) (self->header_extent -
                        gtk_adjustment_get_value (adj) + 0.5),
                0, MAX (height, 0));
}

/* GtkOverlay positions this one child without contributing to the page's
 * measured width. No per-scroll margin or size-request changes are needed:
 * the track list keeps one adjustment and the album rack keeps full width. */
static gboolean
position_search_scrollbar (GtkOverlay *overlay, GtkWidget *child,
                           GdkRectangle *allocation, gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  if (child != self->scrollbar)
    return FALSE;

  gint minimum = 0, natural = 0;
  gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1,
                      &minimum, &natural, NULL, NULL);
  gint viewport_width = gtk_widget_get_width (GTK_WIDGET (overlay));
  gint viewport_height = gtk_widget_get_height (GTK_WIDGET (overlay));
  gint width = MIN (viewport_width, CLAMP (natural, 12, 18));
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (
    spotifygtk_track_list_get_scroller (self->results));
  gint top = search_scrollbar_top (self, adj);
  self->scrollbar_top = top;
  *allocation = (GdkRectangle) {
    .x = MAX (viewport_width - width, 0), .y = top,
    .width = width, .height = MAX (viewport_height - top, 0)
  };
  return TRUE;
}

static void
on_search_header_layout (GObject *object, GParamSpec *pspec,
                         gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  graphene_rect_t bounds;
  if (self->page_header && self->scroll_overlay &&
      gtk_widget_get_parent (self->page_header) &&
      gtk_widget_compute_bounds (self->page_header, self->scroll_overlay,
                                 &bounds)) {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (
      spotifygtk_track_list_get_scroller (self->results));
    self->header_extent = bounds.origin.y + bounds.size.height +
                          gtk_widget_get_margin_bottom (self->page_header) +
                          gtk_adjustment_get_value (adj);
  }
  update_scrollbar_visibility (self);
  (void) object;
  (void) pspec;
}

static void
on_search_scroll_range (GObject *object, GParamSpec *pspec,
                        gpointer user_data)
{
  update_scrollbar_visibility (user_data);
  (void) object;
  (void) pspec;
}

static void
on_search_header_mapped (GtkWidget *widget, gpointer user_data)
{
  on_search_header_layout (G_OBJECT (widget), NULL, user_data);
}

static void
on_search_scroll_value (GtkAdjustment *adj, gpointer user_data)
{
  SpotifyGtkSearchPage *self = user_data;
  /* The scrollbar updates its own thumb from the shared adjustment. Relayout
   * the overlay only while the header boundary actually crosses the screen;
   * no extra allocation pass is needed for the long track-list scroll. */
  if (self->scroll_overlay &&
      search_scrollbar_top (self, adj) != self->scrollbar_top)
    update_scrollbar_visibility (self);
}

/* Search returns a ranked track list. The first distinct albums in that list
 * form the first fifty shelf cards. Keep the records separately so adding
 * playlists later does not rebuild those cards or the already bound rows. */
static void
render_albums (SpotifyGtkSearchPage *self, GPtrArray *tracks)
{
  g_ptr_array_set_size (self->contexts, 0);
  g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < tracks->len && self->contexts->len < SEARCH_ALBUM_LIMIT; i++) {
    const SpotifyNativeTrack *track = g_ptr_array_index (tracks, i);
    if (!track->album_uri || !*track->album_uri ||
        g_hash_table_contains (seen, track->album_uri))
      continue;
    g_hash_table_add (seen, track->album_uri);
    SpotifyNativeTrack *context = g_new0 (SpotifyNativeTrack, 1);
    context->uri = g_strdup (track->album_uri);
    context->name = g_strdup (track->album ? track->album : "Album");
    context->artists = g_strdup (track->artists);
    context->cover_id = g_strdup (track->cover_id);
    context->cover_id_small = g_strdup (track->cover_id_small);
    g_ptr_array_add (self->contexts, context);
  }
  self->album_count = self->contexts->len;
  g_autofree SpotifyGtkCardSpec *cards =
    g_new0 (SpotifyGtkCardSpec, self->album_count);
  g_autoptr(GPtrArray) subtitles = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < self->album_count; i++) {
    const SpotifyNativeTrack *context = g_ptr_array_index (self->contexts, i);
    gchar *subtitle = g_strdup_printf ("Album · %s",
      context->artists ? context->artists : "");
    g_ptr_array_add (subtitles, subtitle);
    cards[i] = (SpotifyGtkCardSpec) { context->uri, context->name,
                                     subtitle, context->cover_id };
  }
  spotifygtk_album_grid_set_cards (self->albums, cards, self->album_count);
}

static void
render_playlists (SpotifyGtkSearchPage *self)
{
  JsonObject *root = self->playlist_answer && JSON_NODE_HOLDS_OBJECT (self->playlist_answer)
    ? json_node_get_object (self->playlist_answer) : NULL;
  JsonObject *search = object_child (object_child (root, "data"), "searchV2");
  JsonArray *items = array_child (object_child (search, "playlists"), "items");
  if (!items)
    items = array_child (object_child (search, "playlistsV2"), "items");
  g_ptr_array_set_size (self->contexts, self->album_count);
  g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  g_autofree gchar *key = search_key (self->result_query);
  guint count = items ? json_array_get_length (items) : 0;
  for (guint i = 0; i < count &&
       self->contexts->len - self->album_count < SEARCH_PLAYLIST_LIMIT; i++) {
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
    SpotifyNativeTrack *context = g_new0 (SpotifyNativeTrack, 1);
    context->uri = g_strdup (uri);
    context->name = g_strdup (name);
    context->artists = g_strdup (owner_name);
    JsonArray *images = array_child (object_child (data, "images"), "items");
    for (guint j = 0; images && j < json_array_get_length (images) &&
         !context->cover_id; j++) {
      JsonNode *image = json_array_get_element (images, j);
      if (!JSON_NODE_HOLDS_OBJECT (image)) continue;
      JsonArray *sources = array_child (json_node_get_object (image), "sources");
      for (guint k = 0; sources && k < json_array_get_length (sources); k++) {
        JsonNode *source = json_array_get_element (sources, k);
        const gchar *url = JSON_NODE_HOLDS_OBJECT (source)
          ? string_child (json_node_get_object (source), "url") : NULL;
        const gchar *prefix = "https://i.scdn.co/image/";
        if (url && g_str_has_prefix (url, prefix)) {
          context->cover_id = g_strdup (url + strlen (prefix));
          break;
        }
      }
    }
    g_ptr_array_add (self->contexts, context);
  }
  guint n = self->contexts->len - self->album_count;
  g_autofree SpotifyGtkCardSpec *cards = g_new0 (SpotifyGtkCardSpec, n);
  g_autoptr(GPtrArray) subtitles = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < n; i++) {
    const SpotifyNativeTrack *context =
      g_ptr_array_index (self->contexts, self->album_count + i);
    gchar *subtitle = g_strdup_printf ("Playlist · %s",
      context->artists ? context->artists : "");
    g_ptr_array_add (subtitles, subtitle);
    cards[i] = (SpotifyGtkCardSpec) { context->uri, context->name,
                                     subtitle, context->cover_id };
  }
  spotifygtk_album_grid_replace_tail (self->albums, self->album_count, cards, n);
  sync_result_layout (self);
  if (n > 0)
    show_message (self, NULL);
  else if (self->album_count == 0 && self->track_count == 0)
    show_message (self, "No results.");
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
on_context_activated (SpotifyGtkTrackList *list,
                      const SpotifyNativeTrack *context,
                      SpotifyGtkSearchPage *self)
{
  g_signal_emit_by_name (self->albums, "album-activated",
                         context->uri, context->name);
  (void) list;
}

static void
on_context_menu (SpotifyGtkTrackList *list,
                 const SpotifyNativeTrack *context,
                 GtkWidget *anchor, gdouble x, gdouble y,
                 SpotifyGtkSearchPage *self)
{
  SpotifyGtkCardSpec card = { context->uri, context->name,
                             context->artists, context->cover_id };
  spotifygtk_album_grid_present_context_menu (self->albums, anchor,
                                               &card, x, y);
  (void) list;
}

static void
on_settings_changed (SpotifyGtkSettings *settings,
                     SpotifyGtkSearchPage *self)
{
  gboolean compact = spotifygtk_settings_get_compact_mode (settings);
  if (compact != self->compact) {
    self->compact = compact;
    sync_result_layout (self);
  }
  gboolean aggressive = spotifygtk_settings_get_aggressive_filtering (settings);
  if (aggressive != self->aggressive) {
    self->aggressive = aggressive;
    if (self->playlist_answer)
      render_playlists (self);
  }
}

static void
on_playlists_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SearchClosure *cl = user_data;
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
      g_message ("search: optional playlist results unavailable: %s",
                 error ? error->message : "empty response");
    if (self->track_count == 0)
      show_message (self, "No results.");
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
    g_ptr_array_set_size (self->contexts, 0);
    self->album_count = 0;
    self->track_count = 0;
    spotifygtk_album_grid_clear (self->albums);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_scroll_to_top (self->results);
    spotifygtk_track_list_set_status (self->results, NULL);
    sync_result_layout (self);
    show_message (self, msg);
    return;
  }

  if (tracks->len == 0) {
    g_ptr_array_set_size (self->contexts, 0);
    self->album_count = 0;
    self->track_count = 0;
    spotifygtk_album_grid_clear (self->albums);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_scroll_to_top (self->results);
    spotifygtk_track_list_set_status (self->results, NULL);
    sync_result_layout (self);
    show_message (self, "No track results. Checking playlists…");
    request_playlists (self, query);
    return;
  }

  /* These are already ranked catalog results from the desktop search query.
   * The old 20-track context needed a word-match filter because it contained
   * playback filler; applying that filter here would throw away Spotify's
   * fuzzy/semantic matches and make the expanded result set look small again. */
  g_autoptr(GPtrArray) shown = g_ptr_array_ref (tracks);
  self->track_count = shown->len;
  if (spotifygtk_settings_get_aggressive_filtering (
        spotifygtk_settings_get_default ())) {
    g_autofree gchar *query_key = search_key (query);
    g_ptr_array_sort_with_data (shown, compare_search_tracks, query_key);
  }

  /* The albums shelf is the distinct albums present in these very results --
   * real matches, grouped, not a second query. */
  show_message (self, NULL);
  /* GtkListView anchors its first surviving track while a preceding virtual
   * header changes height. Growing the album shelf before replacing the old
   * tracks therefore moves the adjustment toward the middle of the page.
   * Empty the old rows first, settle the header's visible structure, then
   * publish the new rows in the same main-loop turn (no blank frame). */
  spotifygtk_track_list_clear (self->results);
  render_albums (self, shown);
  sync_result_layout (self);
  spotifygtk_track_list_set_native_tracks (self->results, shown);
  spotifygtk_track_list_scroll_to_top (self->results);
  request_playlists (self, query);
}

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
  if (self->playlist_request) {
    g_cancellable_cancel (self->playlist_request);
    g_clear_object (&self->playlist_request);
  }
  g_clear_pointer (&self->playlist_answer, json_node_unref);

  if (!query || !*query) {
    set_searching (self, FALSE);
    g_ptr_array_set_size (self->contexts, 0);
    self->album_count = 0;
    self->track_count = 0;
    spotifygtk_album_grid_clear (self->albums);
    spotifygtk_track_list_clear (self->results);
    spotifygtk_track_list_scroll_to_top (self->results);
    spotifygtk_track_list_set_status (self->results, NULL);
    sync_result_layout (self);
    show_message (self, NULL);
    return G_SOURCE_REMOVE;
  }

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    set_searching (self, FALSE);
    show_message (self, "Not signed in yet.");
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
  show_message (self, NULL);
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
  g_clear_handle_id (&self->debounce_id, g_source_remove);
  self->serial++;
  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  if (self->playlist_request)
    g_cancellable_cancel (self->playlist_request);
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
  if (self->playlist_request)
    g_cancellable_cancel (self->playlist_request);
  g_clear_object (&self->playlist_request);
  g_clear_pointer (&self->playlist_answer, json_node_unref);
  g_clear_pointer (&self->contexts, g_ptr_array_unref);
  g_clear_pointer (&self->result_query, g_free);
  g_signal_handlers_disconnect_by_data (spotifygtk_settings_get_default (), self);
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
  self->scrollbar_top = -1;
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  self->contexts = g_ptr_array_new_with_free_func (
    (GDestroyNotify) spotifygtk_native_track_free);
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  self->compact = spotifygtk_settings_get_compact_mode (settings);
  self->aggressive = spotifygtk_settings_get_aggressive_filtering (settings);
  g_signal_connect (settings, "changed", G_CALLBACK (on_settings_changed), self);

  /* The horizontal album shelf and tracks share one virtualised vertical
   * adjustment. The shelf header is its first list item, so neither section
   * can scroll underneath the other. */
  self->results = spotifygtk_track_list_new ();
  /* Inset only track rows, leaving the shelf's horizontal viewport flush
   * with the page edges. The external vertical bar does not reserve width. */
  spotifygtk_track_list_set_content_margins (self->results, 0, 0);
  spotifygtk_track_list_set_row_margins (self->results, 35, 12);
  spotifygtk_track_list_set_show_type (self->results, TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->results), TRUE);
  g_signal_connect (self->results, "track-activated", G_CALLBACK (on_track_activated), self);
  g_signal_connect (self->results, "context-activated",
                    G_CALLBACK (on_context_activated), self);
  g_signal_connect (self->results, "context-menu",
                    G_CALLBACK (on_context_menu), self);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_hexpand (header, TRUE);
  /* Search controls stay fixed like the other collection pages. Only the
   * shelf and tracks belong to the virtual page header/list item, sharing a
   * single vertical adjustment. Keeping the entry out of GtkListView avoids
   * restyling it on every scroll and leaves its focus position stable. */
  gtk_widget_set_size_request (header, -1, 1);
  gtk_widget_set_margin_top (header, 2);
  gtk_widget_set_margin_bottom (header, 10);
  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_hexpand (controls, TRUE);
  gtk_widget_set_margin_top (controls, 24);
  gtk_widget_set_margin_bottom (controls, 10);

  GtkWidget *title = gtk_label_new ("Search");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.5);
  gtk_box_append (GTK_BOX (controls), title);

  self->entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->entry), 460, -1);
  gtk_widget_set_halign (GTK_WIDGET (self->entry), GTK_ALIGN_CENTER);
  gtk_search_entry_set_placeholder_text (self->entry, "Songs, artists, albums");
  g_signal_connect (self->entry, "search-changed", G_CALLBACK (on_search_changed), self);
  g_signal_connect (self->entry, "activate", G_CALLBACK (on_search_activate), self);
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->entry));

  self->message = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (self->message), "dim-label");
  gtk_widget_set_visible (GTK_WIDGET (self->message), FALSE);
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->message));
  gtk_box_append (GTK_BOX (self), controls);

  self->albums_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (self->albums_section, 8);
  gtk_widget_set_margin_bottom (self->albums_section, 10);
  gtk_widget_set_visible (self->albums_section, FALSE);
  self->albums = spotifygtk_album_grid_new_shelf ();
  spotifygtk_album_grid_set_full_card_shelf (self->albums, TRUE);
  /* The shelf has the whole page width. Track-row insets below do not alter
   * its horizontal clip boundary or card sizing. */
  spotifygtk_album_grid_set_content_margins (self->albums, 0, 0);
  gtk_box_append (GTK_BOX (self->albums_section), GTK_WIDGET (self->albums));
  gtk_box_append (GTK_BOX (header), self->albums_section);

  self->page_header = header;
  spotifygtk_track_list_set_page_header (self->results, header);
  spotifygtk_smooth_scroll_set_nested_horizontal (
    spotifygtk_track_list_get_scroller (self->results),
    spotifygtk_album_grid_get_scroller (self->albums));
  spotifygtk_track_list_set_status (self->results, NULL);

  GtkScrolledWindow *scroller =
    spotifygtk_track_list_get_scroller (self->results);
  /* EXTERNAL, unlike NEVER, preserves the viewport's independent size while
   * omitting its internal scrollbar. GTK documents it for a shared/external
   * bar. Both this overlay bar and the list use the same GtkAdjustment. */
  gtk_scrolled_window_set_policy (scroller,
                                  GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
  self->scroll_overlay = gtk_overlay_new ();
  gtk_widget_set_hexpand (self->scroll_overlay, TRUE);
  gtk_widget_set_vexpand (self->scroll_overlay, TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (self->scroll_overlay),
                         GTK_WIDGET (self->results));
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (scroller);
  self->scrollbar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, vadj);
  gtk_widget_add_css_class (self->scrollbar, "search-page-scrollbar");
  gtk_overlay_add_overlay (GTK_OVERLAY (self->scroll_overlay), self->scrollbar);
  gtk_overlay_set_measure_overlay (GTK_OVERLAY (self->scroll_overlay),
                                   self->scrollbar, FALSE);
  gtk_overlay_set_clip_overlay (GTK_OVERLAY (self->scroll_overlay),
                                self->scrollbar, TRUE);
  gtk_widget_set_visible (self->scrollbar, FALSE);
  gtk_box_append (GTK_BOX (self), self->scroll_overlay);

  g_signal_connect (self->scroll_overlay, "get-child-position",
                    G_CALLBACK (position_search_scrollbar), self);
  g_signal_connect_object (vadj, "value-changed",
                           G_CALLBACK (on_search_scroll_value), self, 0);
  g_signal_connect_object (vadj, "notify::upper",
                           G_CALLBACK (on_search_scroll_range), self, 0);
  g_signal_connect_object (vadj, "notify::page-size",
                           G_CALLBACK (on_search_scroll_range), self, 0);
  g_signal_connect_object (header, "notify::height",
                           G_CALLBACK (on_search_header_layout), self, 0);
  g_signal_connect_object (header, "map",
                           G_CALLBACK (on_search_header_mapped), self, 0);
  g_signal_connect_object (self->scroll_overlay, "notify::height",
                           G_CALLBACK (on_search_header_layout), self, 0);
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

/*
 * gui_main.c — deliberately small GTK4 shell for the native engine.
 *
 * This is intentionally independent of the protocol harness. It gives users
 * a dependable place to inspect engine readiness while the long-running
 * playback scheduler is being extracted from main.c. Controls that are not
 * wired to that scheduler stay visibly disabled instead of pretending to
 * work.
 */

#include "config.h"
#include "player_service.h"
#include "spotify/native_auth.h"
#include "api.h"

#include <gtk/gtk.h>
#include <string.h>

typedef struct {
  GtkWidget   *play_button;
  GtkWidget   *pause_button;
  GtkWidget   *stop_button;
  GtkWidget   *playback_status;
  GtkWidget   *track_entry;
  GtkWidget   *search_entry;
  GtkWidget   *bar_play_button;
  GtkWidget   *bar_pause_button;
  GtkWidget   *bar_stop_button;
  GtkWidget   *bar_status;
  GtkWidget   *bar_progress;
  GtkWidget   *search_results;
  GtkWidget   *search_info;
  GtkWidget   *page_stack;
  NativeAuth  *catalog_auth;
  SpotifyApi  *catalog_api;
  GtkWindow   *window;          /* borrowed; owner of this runtime */
  SpotifyNativePlayerService *player;
  guint        search_timeout_id;
  guint64      search_request_serial;
  gchar       *last_search_query;
  gchar       *pending_search_query;
  gchar       *queued_search_query;
  gchar       *search_filter_kind;
  gint64       search_rate_limited_until;
  gboolean     search_request_in_flight;
  gboolean     close_after_stop;
} GuiRuntime;

static void on_play_clicked (GtkButton *button, gpointer user_data);
static void on_search_activated (GtkSearchEntry *entry, gpointer user_data);

static void
gui_runtime_free (GuiRuntime *runtime)
{
  if (runtime->search_timeout_id)
    g_source_remove (runtime->search_timeout_id);
  g_clear_object (&runtime->catalog_api);
  g_clear_object (&runtime->catalog_auth);
  g_clear_object (&runtime->player);
  g_free (runtime->last_search_query);
  g_free (runtime->pending_search_query);
  g_free (runtime->queued_search_query);
  g_free (runtime->search_filter_kind);
  g_free (runtime);
}

static void
set_playback_status (GuiRuntime *runtime, const gchar *message, gboolean running)
{
  gtk_label_set_text (GTK_LABEL (runtime->playback_status), message);
  if (runtime->bar_status)
    gtk_label_set_text (GTK_LABEL (runtime->bar_status), message);
  gtk_widget_set_sensitive (runtime->play_button, !running);
  if (runtime->bar_play_button)
    gtk_widget_set_sensitive (runtime->bar_play_button, !running);
  gtk_widget_set_sensitive (runtime->stop_button, running);
  if (runtime->bar_stop_button)
    gtk_widget_set_sensitive (runtime->bar_stop_button, running);
  gtk_widget_set_sensitive (runtime->pause_button, FALSE);
  if (runtime->bar_pause_button)
    gtk_widget_set_sensitive (runtime->bar_pause_button, FALSE);
}

static void
set_status_message (GuiRuntime *runtime, const gchar *message)
{
  gtk_label_set_text (GTK_LABEL (runtime->playback_status), message);
  if (runtime->bar_status)
    gtk_label_set_text (GTK_LABEL (runtime->bar_status), message);
}

static void
on_player_state_changed (SpotifyNativePlayerService *player, gint state,
                         const gchar *message, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  gboolean active = state == SPOTIFYGTK_PLAYER_CONNECTING ||
                    state == SPOTIFYGTK_PLAYER_BUFFERING ||
                    state == SPOTIFYGTK_PLAYER_PLAYING ||
                    state == SPOTIFYGTK_PLAYER_STOPPING;
  set_playback_status (runtime, message, active);
  gboolean can_pause = state == SPOTIFYGTK_PLAYER_PLAYING;
  gtk_widget_set_sensitive (runtime->pause_button, can_pause);
  if (runtime->bar_pause_button)
    gtk_widget_set_sensitive (runtime->bar_pause_button, can_pause);
  const gchar *pause_label = spotifygtk_player_service_is_paused (runtime->player) ? "Resume" : "Pause";
  gtk_button_set_label (GTK_BUTTON (runtime->pause_button), pause_label);
  if (runtime->bar_pause_button)
    gtk_button_set_label (GTK_BUTTON (runtime->bar_pause_button), pause_label);
  if (runtime->bar_progress) {
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (runtime->bar_progress),
                                   state == SPOTIFYGTK_PLAYER_PLAYING ? 0.65 :
                                   state == SPOTIFYGTK_PLAYER_BUFFERING ? 0.25 : 0.0);
    gtk_progress_bar_set_text (GTK_PROGRESS_BAR (runtime->bar_progress),
                               state == SPOTIFYGTK_PLAYER_PLAYING ? "Playing" : message);
  }
  if (runtime->close_after_stop && !active)
    gtk_window_destroy (runtime->window);
  (void) player;
}

typedef struct {
  GWeakRef window_ref;
  guint64  serial;
  gchar   *query;
} CatalogSearchClosure;

typedef struct {
  GWeakRef window_ref;
  guint64  serial;
  gchar   *kind;
  gchar   *title;
  gchar   *uri;
} CatalogBrowseClosure;

static void
catalog_search_closure_free (CatalogSearchClosure *closure)
{
  g_weak_ref_clear (&closure->window_ref);
  g_free (closure->query);
  g_free (closure);
}

static void
catalog_browse_closure_free (CatalogBrowseClosure *closure)
{
  g_weak_ref_clear (&closure->window_ref);
  g_free (closure->kind);
  g_free (closure->title);
  g_free (closure->uri);
  g_free (closure);
}

static void
clear_search_results (GuiRuntime *runtime)
{
  GtkWidget *child = gtk_widget_get_first_child (runtime->search_results);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    gtk_list_box_remove (GTK_LIST_BOX (runtime->search_results), child);
    child = next;
  }
}

static void
apply_search_filter (GuiRuntime *runtime)
{
  const gchar *filter = runtime->search_filter_kind;
  GtkWidget *child = gtk_widget_get_first_child (runtime->search_results);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    const gchar *kind = g_object_get_data (G_OBJECT (child), "result-kind");
    gboolean visible = !filter || !*filter || g_strcmp0 (kind, filter) == 0;
    gtk_widget_set_visible (child, visible);
    child = next;
  }
}

static JsonObject *
object_member_or_null (JsonObject *object, const gchar *member)
{
  if (!object || !json_object_has_member (object, member))
    return NULL;
  JsonNode *node = json_object_get_member (object, member);
  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

static JsonArray *
array_member_or_null (JsonObject *object, const gchar *member)
{
  if (!object || !json_object_has_member (object, member))
    return NULL;
  JsonNode *node = json_object_get_member (object, member);
  return node && JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
}

static gchar *
artist_names_from_array (JsonArray *artists)
{
  if (!artists || json_array_get_length (artists) == 0)
    return g_strdup ("");

  GString *names = g_string_new (NULL);
  for (guint i = 0; i < json_array_get_length (artists); i++) {
    JsonObject *artist = json_array_get_object_element (artists, i);
    const gchar *name = artist ?
      json_object_get_string_member_with_default (artist, "name", "") : "";
    if (!*name)
      continue;
    if (names->len > 0)
      g_string_append (names, ", ");
    g_string_append (names, name);
  }
  return g_string_free (names, FALSE);
}

static const gchar *
result_icon_for_kind (const gchar *kind)
{
  if (g_strcmp0 (kind, "artist") == 0)
    return "avatar-default-symbolic";
  if (g_strcmp0 (kind, "album") == 0)
    return "media-optical-symbolic";
  if (g_strcmp0 (kind, "playlist") == 0)
    return "folder-music-symbolic";
  return "audio-x-generic-symbolic";
}

static GtkWidget *
build_result_row (const gchar *kind, const gchar *uri, const gchar *title_text,
                  const gchar *subtitle_text, const gchar *detail_text,
                  gboolean playable)
{
  GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (row_box, 8);
  gtk_widget_set_margin_end (row_box, 8);
  gtk_widget_set_margin_top (row_box, 6);
  gtk_widget_set_margin_bottom (row_box, 6);

  GtkWidget *icon = gtk_image_new_from_icon_name (result_icon_for_kind (kind));
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 32);

  GtkWidget *details = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (details, TRUE);

  GtkWidget *top_line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *title = gtk_label_new (title_text ? title_text : "");
  GtkWidget *kind_label = gtk_label_new (kind);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_widget_add_css_class (kind_label, "caption");
  gtk_widget_add_css_class (kind_label, "dim-label");
  gtk_box_append (GTK_BOX (top_line), title);
  gtk_box_append (GTK_BOX (top_line), kind_label);

  GtkWidget *subtitle = gtk_label_new (subtitle_text ? subtitle_text : "");
  gtk_label_set_ellipsize (GTK_LABEL (subtitle), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (subtitle), 0.0f);
  gtk_widget_add_css_class (subtitle, "dim-label");
  gtk_widget_add_css_class (subtitle, "caption");

  GtkWidget *detail = gtk_label_new (detail_text ? detail_text : "");
  gtk_label_set_ellipsize (GTK_LABEL (detail), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (detail), 0.0f);
  gtk_widget_add_css_class (detail, "dim-label");
  gtk_widget_add_css_class (detail, "caption");

  gtk_box_append (GTK_BOX (details), top_line);
  gtk_box_append (GTK_BOX (details), subtitle);
  if (detail_text && *detail_text)
    gtk_box_append (GTK_BOX (details), detail);

  GtkWidget *action = gtk_image_new_from_icon_name (playable ?
                                                    "media-playback-start-symbolic" :
                                                    "go-next-symbolic");
  gtk_widget_set_opacity (action, playable ? 1.0 : 0.45);
  gtk_widget_set_tooltip_text (action, playable ?
                               "Play this track with the native engine." :
                               "Browsable catalog result; native playback starts from tracks.");

  gtk_box_append (GTK_BOX (row_box), icon);
  gtk_box_append (GTK_BOX (row_box), details);
  gtk_box_append (GTK_BOX (row_box), action);

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), row_box);
  g_object_set_data_full (G_OBJECT (row), "result-kind", g_strdup (kind), g_free);
  g_object_set_data_full (G_OBJECT (row), "result-uri", g_strdup (uri ? uri : ""), g_free);
  g_object_set_data_full (G_OBJECT (row), "result-title",
                          g_strdup (title_text ? title_text : ""), g_free);
  if (playable)
    g_object_set_data_full (G_OBJECT (row), "track-uri", g_strdup (uri ? uri : ""), g_free);
  return row;
}

static GtkWidget *
build_track_row (JsonObject *track)
{
  const gchar *name = json_object_get_string_member_with_default (track, "name", "");
  const gchar *uri = json_object_get_string_member_with_default (track, "uri", "");
  g_autofree gchar *artist_names = artist_names_from_array (array_member_or_null (track, "artists"));
  JsonObject *album = object_member_or_null (track, "album");
  const gchar *album_name = album ?
    json_object_get_string_member_with_default (album, "name", "") : "";
  return build_result_row ("track", uri, name, artist_names, album_name, TRUE);
}

static GtkWidget *
build_album_row (JsonObject *album)
{
  const gchar *name = json_object_get_string_member_with_default (album, "name", "");
  const gchar *uri = json_object_get_string_member_with_default (album, "uri", "");
  g_autofree gchar *artist_names = artist_names_from_array (array_member_or_null (album, "artists"));
  const gchar *release_date = json_object_get_string_member_with_default (album, "release_date", "");
  return build_result_row ("album", uri, name, artist_names, release_date, FALSE);
}

static GtkWidget *
build_artist_row (JsonObject *artist)
{
  const gchar *name = json_object_get_string_member_with_default (artist, "name", "");
  const gchar *uri = json_object_get_string_member_with_default (artist, "uri", "");
  g_autofree gchar *detail = NULL;
  JsonObject *followers = object_member_or_null (artist, "followers");
  if (followers && json_object_has_member (followers, "total"))
    detail = g_strdup_printf ("%" G_GINT64_FORMAT " followers",
                              json_object_get_int_member (followers, "total"));
  return build_result_row ("artist", uri, name, "Artist", detail, FALSE);
}

static GtkWidget *
build_playlist_row (JsonObject *playlist)
{
  const gchar *name = json_object_get_string_member_with_default (playlist, "name", "");
  const gchar *uri = json_object_get_string_member_with_default (playlist, "uri", "");
  JsonObject *owner = object_member_or_null (playlist, "owner");
  const gchar *owner_name = owner ?
    json_object_get_string_member_with_default (owner, "display_name", "") : "";
  JsonObject *tracks = object_member_or_null (playlist, "tracks");
  g_autofree gchar *detail = NULL;
  if (tracks && json_object_has_member (tracks, "total"))
    detail = g_strdup_printf ("%" G_GINT64_FORMAT " tracks",
                              json_object_get_int_member (tracks, "total"));
  return build_result_row ("playlist", uri, name, owner_name, detail, FALSE);
}

static guint
append_result_section (GuiRuntime *runtime, JsonObject *result, const gchar *object_name,
                       const gchar *items_name, GtkWidget *(*row_builder) (JsonObject *),
                       gboolean tracks_only)
{
  JsonObject *section_obj = object_member_or_null (result, object_name);
  JsonArray *items = array_member_or_null (section_obj, items_name);
  guint found = 0;
  if (!items)
    return 0;

  for (guint i = 0; i < json_array_get_length (items); i++) {
    JsonObject *item = json_array_get_object_element (items, i);
    if (!item)
      continue;
    const gchar *uri = json_object_get_string_member_with_default (item, "uri", "");
    if (tracks_only && !g_str_has_prefix (uri, "spotify:track:"))
      continue;
    gtk_list_box_append (GTK_LIST_BOX (runtime->search_results), row_builder (item));
    found++;
  }
  return found;
}

static guint
append_track_array (GuiRuntime *runtime, JsonArray *items, gboolean playlist_wrapped)
{
  guint found = 0;
  if (!items)
    return 0;

  for (guint i = 0; i < json_array_get_length (items); i++) {
    JsonObject *item = json_array_get_object_element (items, i);
    if (!item)
      continue;
    JsonObject *track = playlist_wrapped ? object_member_or_null (item, "track") : item;
    if (!track)
      continue;
    const gchar *uri = json_object_get_string_member_with_default (track, "uri", "");
    if (!g_str_has_prefix (uri, "spotify:track:"))
      continue;
    gtk_list_box_append (GTK_LIST_BOX (runtime->search_results), build_track_row (track));
    found++;
  }

  return found;
}

static gchar *
track_uri_from_text (const gchar *text)
{
  if (!text)
    return NULL;

  if (g_str_has_prefix (text, "spotify:track:"))
    return g_strdup (text);

  if (strlen (text) == 22) {
    gboolean base62 = TRUE;
    for (gsize i = 0; i < 22; i++) {
      if (!g_ascii_isalnum (text[i])) {
        base62 = FALSE;
        break;
      }
    }
    if (base62)
      return g_strdup_printf ("spotify:track:%s", text);
  }

  return NULL;
}

static gchar *
spotify_id_from_uri (const gchar *kind, const gchar *uri)
{
  if (!kind || !uri)
    return NULL;

  g_autofree gchar *prefix = g_strdup_printf ("spotify:%s:", kind);
  if (!g_str_has_prefix (uri, prefix))
    return NULL;

  const gchar *id = uri + strlen (prefix);
  if (!*id)
    return NULL;
  return g_strdup (id);
}

static void
select_track_uri (GuiRuntime *runtime, const gchar *uri, gboolean start_playback)
{
  gtk_editable_set_text (GTK_EDITABLE (runtime->track_entry), uri);
  set_status_message (runtime, start_playback ?
                      "Starting selected track..." :
                      "Track selected. Press Play to start the native engine.");
  g_message ("gui: selected native playback track: %s%s",
             uri, start_playback ? " (autoplay)" : "");
  if (start_playback)
    on_play_clicked (NULL, runtime);
}

static gint64
retry_after_seconds_from_error (const GError *error)
{
  if (!error || !error->message)
    return 30;

  const gchar *marker = strstr (error->message, "Retry-After=");
  if (!marker)
    return 30;

  marker += strlen ("Retry-After=");
  gint64 seconds = g_ascii_strtoll (marker, NULL, 10);
  if (seconds <= 0)
    seconds = 30;
  return CLAMP (seconds, 5, 300);
}

static gboolean
run_queued_search (gpointer user_data);

static gboolean
run_debounced_search (gpointer user_data);

static void
schedule_catalog_search (GuiRuntime *runtime)
{
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (runtime->search_entry));
  g_autofree gchar *trimmed = g_strdup (text ? text : "");
  g_strstrip (trimmed);

  if (runtime->search_timeout_id) {
    g_source_remove (runtime->search_timeout_id);
    runtime->search_timeout_id = 0;
  }

  if (!*trimmed) {
    runtime->search_request_serial++;
    g_clear_pointer (&runtime->last_search_query, g_free);
    g_clear_pointer (&runtime->queued_search_query, g_free);
    clear_search_results (runtime);
    gtk_label_set_text (GTK_LABEL (runtime->search_info),
                        "Search for songs, artists, albums, or playlists.");
    return;
  }

  g_autofree gchar *uri = track_uri_from_text (trimmed);
  if (uri) {
    gtk_label_set_text (GTK_LABEL (runtime->search_info),
                        "Press Enter to load this track URI into native playback.");
    return;
  }

  if (strlen (trimmed) < 3) {
    clear_search_results (runtime);
    gtk_label_set_text (GTK_LABEL (runtime->search_info),
                        "Keep typing to search Spotify.");
    return;
  }

  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  if (runtime->search_rate_limited_until > now) {
    g_free (runtime->queued_search_query);
    runtime->queued_search_query = g_strdup (trimmed);
    gint64 wait = runtime->search_rate_limited_until - now;
    g_autofree gchar *message =
      g_strdup_printf ("Spotify is rate limiting catalog search. Waiting %" G_GINT64_FORMAT "s.", wait);
    gtk_label_set_text (GTK_LABEL (runtime->search_info), message);
    g_message ("gui: search suppressed during Spotify API rate-limit cooldown (%" G_GINT64_FORMAT "s left)",
               wait);
    return;
  }

  gtk_label_set_text (GTK_LABEL (runtime->search_info), "Searching...");
  runtime->search_timeout_id = g_timeout_add (900, run_debounced_search, runtime);
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  schedule_catalog_search (user_data);
  (void) entry;
}

static void
switch_to_search_page (GuiRuntime *runtime)
{
  if (runtime->page_stack)
    gtk_stack_set_visible_child_name (GTK_STACK (runtime->page_stack), "search");
}

static void
on_header_search_focus_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  if (gtk_widget_has_focus (GTK_WIDGET (object)))
    switch_to_search_page (user_data);
  (void) pspec;
}

static GtkWidget *
build_search_page (GuiRuntime *runtime);

static GtkWidget *
build_header_search (GuiRuntime *runtime)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_size_request (box, 360, -1);

  GtkWidget *icon = gtk_image_new_from_icon_name ("system-search-symbolic");
  gtk_box_append (GTK_BOX (box), icon);

  runtime->search_entry = gtk_search_entry_new ();
  gtk_widget_set_hexpand (runtime->search_entry, TRUE);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (runtime->search_entry),
                                         "Search Spotify");
  gtk_widget_set_tooltip_text (runtime->search_entry,
                               "Search tracks, artists, albums, and playlists.");
  gtk_box_append (GTK_BOX (box), runtime->search_entry);
  g_signal_connect (runtime->search_entry, "activate",
                    G_CALLBACK (on_search_activated), runtime);
  g_signal_connect (runtime->search_entry, "search-changed",
                    G_CALLBACK (on_search_changed), runtime);
  g_signal_connect (runtime->search_entry, "notify::has-focus",
                    G_CALLBACK (on_header_search_focus_changed), runtime);
  return box;
}

static void
on_catalog_search_results (SpotifyApi *api, JsonObject *result, GError *error,
                           gpointer user_data)
{
  CatalogSearchClosure *closure = user_data;
  GuiRuntime *runtime = NULL;
  g_autoptr(GtkWindow) window = g_weak_ref_get (&closure->window_ref);
  if (!window)
    goto done;

  runtime = g_object_get_data (G_OBJECT (window), "gui-runtime");
  if (!runtime)
    goto done;
  if (closure->serial != runtime->search_request_serial) {
    g_message ("gui: ignoring stale search response for '%s'", closure->query);
    goto done;
  }
  runtime->search_request_in_flight = FALSE;
  clear_search_results (runtime);

  if (error) {
    g_warning ("Search request failed: %s", error->message);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BUSY)) {
      gint64 wait = retry_after_seconds_from_error (error);
      runtime->search_rate_limited_until = g_get_real_time () / G_USEC_PER_SEC + wait;
      g_clear_pointer (&runtime->last_search_query, g_free);
      g_autofree gchar *message =
        g_strdup_printf ("Spotify API rate limit reached. Search will retry after %" G_GINT64_FORMAT "s.", wait);
      gtk_label_set_text (GTK_LABEL (runtime->search_info), message);
      g_message ("gui: Spotify API rate limited search '%s'; cooldown=%" G_GINT64_FORMAT "s",
                 closure->query, wait);
    } else {
      gtk_label_set_text (GTK_LABEL (runtime->search_info), error->message);
    }
    goto done;
  }

  if (!result) {
    gtk_label_set_text (GTK_LABEL (runtime->search_info), "Search returned no response.");
    goto done;
  }

  guint tracks = append_result_section (runtime, result, "tracks", "items",
                                        build_track_row, TRUE);
  guint artists = append_result_section (runtime, result, "artists", "items",
                                         build_artist_row, FALSE);
  guint albums = append_result_section (runtime, result, "albums", "items",
                                        build_album_row, FALSE);
  guint playlists = append_result_section (runtime, result, "playlists", "items",
                                           build_playlist_row, FALSE);
  guint found = tracks + artists + albums + playlists;
  apply_search_filter (runtime);

  g_message ("gui: search '%s' returned %u tracks, %u artists, %u albums, %u playlists",
             closure->query, tracks, artists, albums, playlists);
  gtk_label_set_text (GTK_LABEL (runtime->search_info), found ?
                      "Activate a track to play it with the native engine." :
                      "No results found.");

done:
  if (window && runtime && !runtime->search_request_in_flight &&
      runtime->queued_search_query && runtime->search_timeout_id == 0) {
    runtime->search_timeout_id = g_timeout_add (1000, run_queued_search, runtime);
  }
  catalog_search_closure_free (closure);
  (void) api;
}

static void
start_catalog_search (GuiRuntime *runtime, const gchar *query)
{
  g_autofree gchar *trimmed = g_strdup (query ? query : "");
  g_strstrip (trimmed);
  if (!*trimmed)
    return;

  switch_to_search_page (runtime);
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  if (runtime->search_rate_limited_until > now) {
    g_free (runtime->queued_search_query);
    runtime->queued_search_query = g_strdup (trimmed);
    gint64 wait = runtime->search_rate_limited_until - now;
    g_autofree gchar *message =
      g_strdup_printf ("Spotify is rate limiting catalog search. Waiting %" G_GINT64_FORMAT "s.", wait);
    gtk_label_set_text (GTK_LABEL (runtime->search_info), message);
    g_message ("gui: queued search '%s' during rate-limit cooldown (%" G_GINT64_FORMAT "s left)",
               trimmed, wait);
    return;
  }

  if (!runtime->catalog_auth)
    runtime->catalog_auth = native_auth_new ();
  if (!runtime->catalog_api)
    runtime->catalog_api = spotifygtk_api_new_with_bearer_token (NULL);
  if (!native_auth_has_valid_token (runtime->catalog_auth)) {
    g_free (runtime->pending_search_query);
    runtime->pending_search_query = g_strdup (trimmed);
    gtk_label_set_text (GTK_LABEL (runtime->search_info),
                        "Signing in to Spotify for catalog search...");
    g_message ("gui: catalog search needs native auth; refreshing or starting browser login");
    native_auth_refresh (runtime->catalog_auth);
    return;
  }
  spotifygtk_api_set_bearer_token (runtime->catalog_api,
                                   native_auth_get_token (runtime->catalog_auth));
  if (runtime->search_request_in_flight) {
    g_free (runtime->queued_search_query);
    runtime->queued_search_query = g_strdup (trimmed);
    gtk_label_set_text (GTK_LABEL (runtime->search_info),
                        "Finishing previous search before sending the latest query...");
    g_message ("gui: coalescing search '%s' while previous request is in flight", trimmed);
    return;
  }
  if (g_strcmp0 (runtime->last_search_query, trimmed) == 0) {
    g_message ("gui: skipping duplicate search for '%s'", trimmed);
    return;
  }
  g_free (runtime->last_search_query);
  runtime->last_search_query = g_strdup (trimmed);
  runtime->search_request_serial++;
  clear_search_results (runtime);
  gtk_label_set_text (GTK_LABEL (runtime->search_info), "Searching Spotify...");
  g_message ("gui: searching Spotify catalog for '%s'", trimmed);
  runtime->search_request_in_flight = TRUE;
  CatalogSearchClosure *closure = g_new0 (CatalogSearchClosure, 1);
  closure->serial = runtime->search_request_serial;
  closure->query = g_strdup (trimmed);
  g_weak_ref_init (&closure->window_ref, runtime->window);
  spotifygtk_api_search (runtime->catalog_api, trimmed, "track,artist,album,playlist",
                         on_catalog_search_results, closure);
}

static void
on_catalog_browse_tracks (SpotifyApi *api, JsonObject *result, GError *error,
                          gpointer user_data)
{
  CatalogBrowseClosure *closure = user_data;
  g_autoptr(GtkWindow) window = g_weak_ref_get (&closure->window_ref);
  if (!window)
    goto done;

  GuiRuntime *runtime = g_object_get_data (G_OBJECT (window), "gui-runtime");
  if (!runtime)
    goto done;
  if (closure->serial != runtime->search_request_serial) {
    g_message ("gui: ignoring stale browse response for %s '%s'",
               closure->kind, closure->title);
    goto done;
  }

  clear_search_results (runtime);
  if (error) {
    g_warning ("Catalog browse failed for %s %s: %s",
               closure->kind, closure->uri, error->message);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BUSY)) {
      gint64 wait = retry_after_seconds_from_error (error);
      runtime->search_rate_limited_until = g_get_real_time () / G_USEC_PER_SEC + wait;
      g_autofree gchar *message =
        g_strdup_printf ("Spotify API rate limit reached. Try again after %" G_GINT64_FORMAT "s.", wait);
      gtk_label_set_text (GTK_LABEL (runtime->search_info), message);
      g_message ("gui: Spotify API rate limited catalog browse %s '%s'; cooldown=%" G_GINT64_FORMAT "s",
                 closure->kind, closure->title, wait);
    } else {
      gtk_label_set_text (GTK_LABEL (runtime->search_info), error->message);
    }
    goto done;
  }

  guint found = 0;
  if (g_strcmp0 (closure->kind, "playlist") == 0) {
    found = append_track_array (runtime, array_member_or_null (result, "items"), TRUE);
  } else if (g_strcmp0 (closure->kind, "artist") == 0) {
    found = append_track_array (runtime, array_member_or_null (result, "tracks"), FALSE);
  } else {
    found = append_track_array (runtime, array_member_or_null (result, "items"), FALSE);
  }

  g_autofree gchar *message = found ?
    g_strdup_printf ("%s - %u playable track%s. Activate a track to play it.",
                     closure->title, found, found == 1 ? "" : "s") :
    g_strdup_printf ("%s - no playable tracks returned.", closure->title);
  gtk_label_set_text (GTK_LABEL (runtime->search_info), message);
  g_message ("gui: opened %s '%s' (%s), loaded %u playable track(s)",
             closure->kind, closure->title, closure->uri, found);

done:
  catalog_browse_closure_free (closure);
  (void) api;
}

static void
open_catalog_result (GuiRuntime *runtime, const gchar *kind,
                     const gchar *uri, const gchar *title)
{
  if (!kind || !uri || !*uri)
    return;

  if (!native_auth_has_valid_token (runtime->catalog_auth)) {
    g_free (runtime->pending_search_query);
    runtime->pending_search_query = g_strdup (title && *title ? title : uri);
    gtk_label_set_text (GTK_LABEL (runtime->search_info),
                        "Signing in to Spotify before opening catalog results...");
    g_message ("gui: catalog browse needs native auth; refreshing or starting browser login");
    native_auth_refresh (runtime->catalog_auth);
    return;
  }
  spotifygtk_api_set_bearer_token (runtime->catalog_api,
                                   native_auth_get_token (runtime->catalog_auth));

  g_autofree gchar *id = spotify_id_from_uri (kind, uri);
  if (!id) {
    g_warning ("gui: cannot open catalog result with unsupported uri: kind=%s uri=%s",
               kind, uri);
    set_status_message (runtime, "Unsupported Spotify result URI.");
    return;
  }

  runtime->search_request_serial++;
  g_clear_pointer (&runtime->last_search_query, g_free);
  clear_search_results (runtime);
  g_autofree gchar *loading = g_strdup_printf ("Opening %s...", title && *title ? title : uri);
  gtk_label_set_text (GTK_LABEL (runtime->search_info), loading);
  g_message ("gui: opening catalog %s '%s' (%s)", kind, title ? title : "", uri);

  CatalogBrowseClosure *closure = g_new0 (CatalogBrowseClosure, 1);
  closure->serial = runtime->search_request_serial;
  closure->kind = g_strdup (kind);
  closure->title = g_strdup (title && *title ? title : uri);
  closure->uri = g_strdup (uri);
  g_weak_ref_init (&closure->window_ref, runtime->window);

  if (g_strcmp0 (kind, "album") == 0)
    spotifygtk_api_get_album_tracks (runtime->catalog_api, id,
                                     on_catalog_browse_tracks, closure);
  else if (g_strcmp0 (kind, "artist") == 0)
    spotifygtk_api_get_artist_top_tracks (runtime->catalog_api, id,
                                          on_catalog_browse_tracks, closure);
  else if (g_strcmp0 (kind, "playlist") == 0)
    spotifygtk_api_get_playlist_tracks (runtime->catalog_api, id,
                                        on_catalog_browse_tracks, closure);
  else {
    g_warning ("gui: no browse handler for catalog result kind=%s uri=%s", kind, uri);
    catalog_browse_closure_free (closure);
  }
}

static gboolean
run_debounced_search (gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  runtime->search_timeout_id = 0;
  const gchar *query = gtk_editable_get_text (GTK_EDITABLE (runtime->search_entry));
  start_catalog_search (runtime, query);
  return G_SOURCE_REMOVE;
}

static gboolean
run_queued_search (gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  runtime->search_timeout_id = 0;
  if (!runtime->queued_search_query)
    return G_SOURCE_REMOVE;

  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  if (runtime->search_rate_limited_until > now) {
    gint64 wait = runtime->search_rate_limited_until - now;
    g_autofree gchar *message =
      g_strdup_printf ("Spotify is rate limiting catalog search. Waiting %" G_GINT64_FORMAT "s.", wait);
    gtk_label_set_text (GTK_LABEL (runtime->search_info), message);
    runtime->search_timeout_id =
      g_timeout_add_seconds ((guint) CLAMP (wait, 1, 60), run_queued_search, runtime);
    return G_SOURCE_REMOVE;
  }

  g_autofree gchar *query = g_steal_pointer (&runtime->queued_search_query);
  g_message ("gui: draining queued search '%s'", query);
  start_catalog_search (runtime, query);
  return G_SOURCE_REMOVE;
}

static void
on_catalog_auth_completed (NativeAuth *auth, gboolean success, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  if (success) {
    spotifygtk_api_set_bearer_token (runtime->catalog_api, native_auth_get_token (auth));
    if (runtime->pending_search_query) {
      g_autofree gchar *query = g_steal_pointer (&runtime->pending_search_query);
      g_message ("gui: native auth completed; retrying pending catalog search '%s'", query);
      start_catalog_search (runtime, query);
      return;
    }
  }
  gtk_label_set_text (GTK_LABEL (runtime->search_info),
                      success ? "Signed in. Search for songs, artists, albums, or playlists."
                              : "Spotify sign-in failed; check the terminal diagnostics.");
  (void) auth;
}

static void
on_search_activated (GtkSearchEntry *entry, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  g_autofree gchar *uri = track_uri_from_text (text);

  if (!uri) {
    start_catalog_search (runtime, text);
    return;
  }

  select_track_uri (runtime, uri, FALSE);
}

static void
on_result_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  const gchar *uri = g_object_get_data (G_OBJECT (row), "track-uri");
  if (!gtk_widget_get_visible (GTK_WIDGET (row)))
    return;
  if (!uri || !*uri) {
    const gchar *kind = g_object_get_data (G_OBJECT (row), "result-kind");
    const gchar *result_uri = g_object_get_data (G_OBJECT (row), "result-uri");
    const gchar *title = g_object_get_data (G_OBJECT (row), "result-title");
    open_catalog_result (runtime, kind, result_uri, title);
    return;
  }
  select_track_uri (runtime, uri, TRUE);
  (void) box;
}

static void
on_search_filter_clicked (GtkButton *button, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  const gchar *kind = g_object_get_data (G_OBJECT (button), "filter-kind");
  g_free (runtime->search_filter_kind);
  runtime->search_filter_kind = kind && *kind ? g_strdup (kind) : NULL;
  apply_search_filter (runtime);
  g_message ("gui: search filter changed to %s",
             runtime->search_filter_kind ? runtime->search_filter_kind : "all");
}

static GtkWidget *
build_search_filter_button (GuiRuntime *runtime, const gchar *label, const gchar *kind)
{
  GtkWidget *button = gtk_button_new_with_label (label);
  g_object_set_data (G_OBJECT (button), "filter-kind", (gpointer) kind);
  g_signal_connect (button, "clicked", G_CALLBACK (on_search_filter_clicked), runtime);
  return button;
}

static void
on_play_clicked (GtkButton *button, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  g_autoptr(GError) error = NULL;
  const gchar *track_uri = gtk_editable_get_text (GTK_EDITABLE (runtime->track_entry));
  if (!spotifygtk_player_service_start_uri (runtime->player, track_uri, &error)) {
    g_warning ("gui: failed to start playback service: %s", error->message);
    set_playback_status (runtime, "Could not start playback. Check the terminal diagnostics.", FALSE);
  }
  (void) button;
}

static void
on_stop_clicked (GtkButton *button, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  spotifygtk_player_service_stop (runtime->player);
  (void) button;
}

static void
on_pause_clicked (GtkButton *button, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  if (spotifygtk_player_service_is_paused (runtime->player))
    spotifygtk_player_service_resume (runtime->player);
  else
    spotifygtk_player_service_pause (runtime->player);
  (void) button;
}

static gboolean
on_window_close_request (GtkWindow *window, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  (void) window;
  if (!spotifygtk_player_service_is_active (runtime->player))
    return FALSE;

  g_message ("gui: window close requested while playback is active; stopping player service first");
  runtime->close_after_stop = TRUE;
  set_playback_status (runtime, "Stop requested; finishing the current engine operation…", TRUE);
  spotifygtk_player_service_stop (runtime->player);
  return TRUE;
}

static GtkWidget *
status_row (const gchar *title, const gchar *detail)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top (box, 8);
  gtk_widget_set_margin_bottom (box, 8);

  GtkWidget *heading = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (heading), 0.0f);
  gtk_widget_add_css_class (heading, "heading");
  gtk_box_append (GTK_BOX (box), heading);

  GtkWidget *description = gtk_label_new (detail);
  gtk_label_set_xalign (GTK_LABEL (description), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (description), TRUE);
  gtk_widget_add_css_class (description, "dim-label");
  gtk_box_append (GTK_BOX (box), description);
  return box;
}

static GtkWidget *
section (const gchar *title, GtkWidget *content)
{
  GtkWidget *frame = gtk_frame_new (title);
  gtk_widget_set_margin_bottom (frame, 12);
  gtk_frame_set_child (GTK_FRAME (frame), content);
  return frame;
}

static GtkWidget *
build_now_playing_page (GuiRuntime *runtime)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (page, 24);
  gtk_widget_set_margin_end (page, 24);
  gtk_widget_set_margin_top (page, 24);
  gtk_widget_set_margin_bottom (page, 24);

  GtkWidget *title = gtk_label_new ("Now Playing");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_widget_add_css_class (title, "title-1");
  gtk_box_append (GTK_BOX (page), title);

  GtkWidget *empty = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (empty, 16);
  gtk_widget_set_margin_end (empty, 16);
  gtk_widget_set_margin_top (empty, 16);
  gtk_widget_set_margin_bottom (empty, 16);
  GtkWidget *heading = gtk_label_new ("No track selected");
  gtk_label_set_xalign (GTK_LABEL (heading), 0.0f);
  gtk_widget_add_css_class (heading, "heading");
  GtkWidget *detail = gtk_label_new ("Enter a Spotify track URI and Play to launch the validated native playback engine.");
  gtk_label_set_xalign (GTK_LABEL (detail), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (detail), TRUE);
  gtk_widget_add_css_class (detail, "dim-label");
  gtk_box_append (GTK_BOX (empty), heading);
  gtk_box_append (GTK_BOX (empty), detail);
  runtime->track_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (runtime->track_entry),
                                  "Spotify URI (spotify:track:...)");
  gtk_editable_set_text (GTK_EDITABLE (runtime->track_entry),
                         "spotify:track:6rqhFgbbKwnb9MLmUQDhG6");
  gtk_widget_set_hexpand (runtime->track_entry, TRUE);
  gtk_box_append (GTK_BOX (empty), runtime->track_entry);
  gtk_box_append (GTK_BOX (page), section ("Playback", empty));

  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *previous = gtk_button_new_with_label ("Previous");
  runtime->play_button = gtk_button_new_with_label ("Play test track");
  runtime->pause_button = gtk_button_new_with_label ("Pause");
  runtime->stop_button = gtk_button_new_with_label ("Stop");
  GtkWidget *next = gtk_button_new_with_label ("Next");
  gtk_widget_set_sensitive (previous, FALSE);
  gtk_widget_set_sensitive (runtime->pause_button, FALSE);
  gtk_widget_set_sensitive (runtime->stop_button, FALSE);
  gtk_widget_set_sensitive (next, FALSE);
  gtk_widget_set_tooltip_text (previous, "Queue controls will be enabled with the in-process scheduler.");
  gtk_widget_set_tooltip_text (runtime->pause_button, "Pause keeps buffered PCM in memory; Resume continues output.");
  g_signal_connect (runtime->play_button, "clicked", G_CALLBACK (on_play_clicked), runtime);
  g_signal_connect (runtime->pause_button, "clicked", G_CALLBACK (on_pause_clicked), runtime);
  g_signal_connect (runtime->stop_button, "clicked", G_CALLBACK (on_stop_clicked), runtime);
  gtk_box_append (GTK_BOX (controls), previous);
  gtk_box_append (GTK_BOX (controls), runtime->play_button);
  gtk_box_append (GTK_BOX (controls), runtime->pause_button);
  gtk_box_append (GTK_BOX (controls), runtime->stop_button);
  gtk_box_append (GTK_BOX (controls), next);
  gtk_box_append (GTK_BOX (page), controls);

  runtime->playback_status = gtk_label_new ("Ready to start the native playback engine.");
  gtk_label_set_xalign (GTK_LABEL (runtime->playback_status), 0.0f);
  gtk_widget_add_css_class (runtime->playback_status, "dim-label");
  gtk_box_append (GTK_BOX (page), runtime->playback_status);
  return page;
}

static GtkWidget *
build_search_page (GuiRuntime *runtime)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (page, 24);
  gtk_widget_set_margin_end (page, 24);
  gtk_widget_set_margin_top (page, 24);
  gtk_widget_set_margin_bottom (page, 24);

  GtkWidget *title = gtk_label_new ("Search");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_widget_add_css_class (title, "title-1");
  gtk_box_append (GTK_BOX (page), title);

  runtime->search_info = gtk_label_new ("Search for songs, artists, albums, or playlists.");
  gtk_label_set_xalign (GTK_LABEL (runtime->search_info), 0.0f);
  gtk_widget_add_css_class (runtime->search_info, "dim-label");
  gtk_box_append (GTK_BOX (page), runtime->search_info);

  GtkWidget *filters = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (filters), build_search_filter_button (runtime, "All", NULL));
  gtk_box_append (GTK_BOX (filters), build_search_filter_button (runtime, "Songs", "track"));
  gtk_box_append (GTK_BOX (filters), build_search_filter_button (runtime, "Artists", "artist"));
  gtk_box_append (GTK_BOX (filters), build_search_filter_button (runtime, "Albums", "album"));
  gtk_box_append (GTK_BOX (filters), build_search_filter_button (runtime, "Playlists", "playlist"));
  gtk_box_append (GTK_BOX (page), filters);

  runtime->search_results = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (runtime->search_results), GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (runtime->search_results, "boxed-list");
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), runtime->search_results);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);

  g_signal_connect (runtime->search_results, "row-activated",
                    G_CALLBACK (on_result_row_activated), runtime);
  return page;
}

static GtkWidget *
build_library_page (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (page, 24);
  gtk_widget_set_margin_end (page, 24);
  gtk_widget_set_margin_top (page, 24);
  gtk_widget_set_margin_bottom (page, 24);

  GtkWidget *title = gtk_label_new ("Library");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_widget_add_css_class (title, "title-1");
  gtk_box_append (GTK_BOX (page), title);

  GtkWidget *saved = status_row ("Saved tracks", "Your liked songs and playlists will appear here once library synchronization is connected.");
  gtk_box_append (GTK_BOX (page), section ("Your music", saved));
  GtkWidget *hint = status_row ("Search first", "Use Search to find a track, activate a result, and send it to the native playback engine.");
  gtk_box_append (GTK_BOX (page), section ("Getting started", hint));
  return page;
}

static GtkWidget *
build_engine_page (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (page, 24);
  gtk_widget_set_margin_end (page, 24);
  gtk_widget_set_margin_top (page, 24);
  gtk_widget_set_margin_bottom (page, 24);

  GtkWidget *title = gtk_label_new ("Engine");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_widget_add_css_class (title, "title-1");
  gtk_widget_set_margin_bottom (title, 18);
  gtk_box_append (GTK_BOX (page), title);

  GtkWidget *status_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (status_box, 16);
  gtk_widget_set_margin_end (status_box, 16);
  gtk_widget_set_margin_top (status_box, 8);
  gtk_widget_set_margin_bottom (status_box, 8);
  gtk_box_append (GTK_BOX (status_box), status_row ("Playback pipeline", "AP login, audio-key retrieval, CDN decrypt, Ogg/Vorbis decode, and local PCM output are live-validated."));
  gtk_box_append (GTK_BOX (status_box), status_row ("Playback model", "Incremental CDN ranges now feed the decoder while audio is playing. Queueing, seeking, and scheduler-backed controls are next."));
  gtk_box_append (GTK_BOX (page), section ("Engine status", status_box));
  return page;
}

static GtkWidget *
build_play_bar (GuiRuntime *runtime)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start (bar, 12);
  gtk_widget_set_margin_end (bar, 12);
  gtk_widget_set_margin_top (bar, 8);
  gtk_widget_set_margin_bottom (bar, 8);
  gtk_widget_add_css_class (bar, "toolbar");

  GtkWidget *track = gtk_label_new ("No track selected");
  gtk_label_set_xalign (GTK_LABEL (track), 0.0f);
  gtk_widget_set_size_request (track, 170, -1);
  gtk_widget_set_hexpand (track, FALSE);
  gtk_widget_add_css_class (track, "heading");
  gtk_box_append (GTK_BOX (bar), track);

  GtkWidget *previous = gtk_button_new_with_label ("Previous");
  gtk_widget_set_sensitive (previous, FALSE);
  gtk_widget_set_tooltip_text (previous, "Previous track will be enabled with queue support.");
  gtk_box_append (GTK_BOX (bar), previous);

  runtime->bar_play_button = gtk_button_new_with_label ("Play");
  runtime->bar_pause_button = gtk_button_new_with_label ("Pause");
  runtime->bar_stop_button = gtk_button_new_with_label ("Stop");
  gtk_widget_set_sensitive (runtime->bar_pause_button, FALSE);
  gtk_widget_set_sensitive (runtime->bar_stop_button, FALSE);
  g_signal_connect (runtime->bar_play_button, "clicked", G_CALLBACK (on_play_clicked), runtime);
  g_signal_connect (runtime->bar_pause_button, "clicked", G_CALLBACK (on_pause_clicked), runtime);
  g_signal_connect (runtime->bar_stop_button, "clicked", G_CALLBACK (on_stop_clicked), runtime);
  gtk_box_append (GTK_BOX (bar), runtime->bar_play_button);
  gtk_box_append (GTK_BOX (bar), runtime->bar_pause_button);
  gtk_box_append (GTK_BOX (bar), runtime->bar_stop_button);

  runtime->bar_progress = gtk_progress_bar_new ();
  gtk_widget_set_hexpand (runtime->bar_progress, TRUE);
  gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (runtime->bar_progress), TRUE);
  gtk_progress_bar_set_text (GTK_PROGRESS_BAR (runtime->bar_progress), "Ready");
  gtk_box_append (GTK_BOX (bar), runtime->bar_progress);

  runtime->bar_status = gtk_label_new ("Ready");
  gtk_label_set_xalign (GTK_LABEL (runtime->bar_status), 0.0f);
  gtk_widget_set_size_request (runtime->bar_status, 180, -1);
  gtk_widget_add_css_class (runtime->bar_status, "dim-label");
  gtk_box_append (GTK_BOX (bar), runtime->bar_status);
  return bar;
}

static GtkWidget *
build_settings_page (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (page, 24);
  gtk_widget_set_margin_end (page, 24);
  gtk_widget_set_margin_top (page, 24);
  gtk_widget_set_margin_bottom (page, 24);

  GtkWidget *title = gtk_label_new ("Settings");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_widget_add_css_class (title, "title-1");
  gtk_widget_set_margin_bottom (title, 18);
  gtk_box_append (GTK_BOX (page), title);

  GtkWidget *details = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (details, 16);
  gtk_widget_set_margin_end (details, 16);
  gtk_widget_set_margin_top (details, 8);
  gtk_widget_set_margin_bottom (details, 8);
  gtk_box_append (GTK_BOX (details), status_row ("Build profile", APP_PROFILE));
  gtk_box_append (GTK_BOX (details), status_row ("Audio backends", HAVE_PULSE ? "PulseAudio available; ALSA fallback available." : "ALSA fallback available."));
  gtk_box_append (GTK_BOX (details), status_row ("Application version", APP_VERSION));
  gtk_box_append (GTK_BOX (page), section ("Runtime", details));
  return page;
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
  GtkWindow *window = g_object_get_data (G_OBJECT (app), "main-window");
  if (window) {
    gtk_window_present (window);
    return;
  }

  window = GTK_WINDOW (gtk_application_window_new (app));
  gtk_window_set_title (window, "SpotifyGTK Native");
  gtk_window_set_default_size (window, 760, 520);
  gtk_window_set_icon_name (window, APP_ID);

  GuiRuntime *runtime = g_new0 (GuiRuntime, 1);
  runtime->window = window;
  runtime->player = spotifygtk_player_service_new ();
  runtime->catalog_auth = native_auth_new ();
  runtime->catalog_api = spotifygtk_api_new_with_bearer_token (NULL);

  GtkWidget *header = gtk_header_bar_new ();
  GtkWidget *header_title = gtk_label_new ("SpotifyGTK Native");
  gtk_widget_add_css_class (header_title, "title");
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), header_title);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), build_header_search (runtime));
  gtk_window_set_titlebar (window, header);

  GtkWidget *stack = gtk_stack_new ();
  runtime->page_stack = stack;
  gtk_stack_set_transition_type (GTK_STACK (stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  g_signal_connect (runtime->catalog_auth, "completed",
                    G_CALLBACK (on_catalog_auth_completed), runtime);
  g_signal_connect (runtime->player, "state-changed", G_CALLBACK (on_player_state_changed), runtime);
  gtk_stack_add_titled (GTK_STACK (stack), build_now_playing_page (runtime), "now-playing", "Now Playing");
  gtk_stack_add_titled (GTK_STACK (stack), build_search_page (runtime), "search", "Search");
  gtk_stack_add_titled (GTK_STACK (stack), build_library_page (), "library", "Library");
  gtk_stack_add_titled (GTK_STACK (stack), build_engine_page (), "engine", "Engine");
  gtk_stack_add_titled (GTK_STACK (stack), build_settings_page (), "settings", "Settings");

  GtkWidget *sidebar = gtk_stack_sidebar_new ();
  gtk_stack_sidebar_set_stack (GTK_STACK_SIDEBAR (sidebar), GTK_STACK (stack));
  gtk_widget_set_size_request (sidebar, 180, -1);
  gtk_widget_set_margin_top (sidebar, 12);
  gtk_widget_set_margin_bottom (sidebar, 12);

  GtkWidget *layout = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (layout), sidebar);
  gtk_box_append (GTK_BOX (layout), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  gtk_box_append (GTK_BOX (layout), stack);
  gtk_widget_set_hexpand (stack, TRUE);
  gtk_widget_set_vexpand (stack, TRUE);

  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (layout, TRUE);
  gtk_box_append (GTK_BOX (root), layout);
  gtk_box_append (GTK_BOX (root), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (root), build_play_bar (runtime));
  gtk_window_set_child (window, root);
  g_object_set_data (G_OBJECT (app), "main-window", window);
  g_object_set_data_full (G_OBJECT (window), "gui-runtime", runtime, (GDestroyNotify) gui_runtime_free);
  g_signal_connect (window, "close-request", G_CALLBACK (on_window_close_request), runtime);
  gtk_window_present (window);
  (void) user_data;
}

int
main (int argc, char *argv[])
{
  GtkApplication *app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}

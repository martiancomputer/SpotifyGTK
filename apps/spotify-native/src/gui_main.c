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

#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
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
  SoupSession *search_session;
  GCancellable *search_cancellable;
  NativeAuth  *search_auth;
  GtkWindow   *window;          /* borrowed; owner of this runtime */
  SpotifyNativePlayerService *player;
  gboolean     close_after_stop;
} GuiRuntime;

static void
gui_runtime_free (GuiRuntime *runtime)
{
  if (runtime->search_cancellable)
    g_cancellable_cancel (runtime->search_cancellable);
  g_clear_object (&runtime->search_cancellable);
  g_clear_object (&runtime->search_session);
  g_clear_object (&runtime->search_auth);
  g_clear_object (&runtime->player);
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
  SoupMessage *message;
} CatalogSearchClosure;

static void
catalog_search_closure_free (CatalogSearchClosure *closure)
{
  g_weak_ref_clear (&closure->window_ref);
  g_clear_object (&closure->message);
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
on_search_result_clicked (GtkButton *button, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  const gchar *uri = g_object_get_data (G_OBJECT (button), "track-uri");
  if (!uri)
    return;
  gtk_editable_set_text (GTK_EDITABLE (runtime->track_entry), uri);
  gtk_editable_set_text (GTK_EDITABLE (runtime->search_entry), uri);
  set_status_message (runtime, "Track selected. Press Play to start the native engine.");
  g_message ("gui: selected catalog result: %s", uri);
}

static void
on_catalog_search_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CatalogSearchClosure *closure = user_data;
  g_autoptr(GError) error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  g_autoptr(GtkWindow) window = g_weak_ref_get (&closure->window_ref);
  if (!window)
    goto done;

  GuiRuntime *runtime = g_object_get_data (G_OBJECT (window), "gui-runtime");
  if (!runtime)
    goto done;
  clear_search_results (runtime);

  if (!bytes) {
    gtk_label_set_text (GTK_LABEL (runtime->search_info),
                        error ? error->message : "Search request failed.");
    goto done;
  }

  gsize length = 0;
  const gchar *body = g_bytes_get_data (bytes, &length);
  guint status = soup_message_get_status (closure->message);
  if (status >= 400) {
    gtk_label_set_text (GTK_LABEL (runtime->search_info),
                        "Spotify rejected the search request; sign in again if needed.");
    g_bytes_unref (bytes);
    goto done;
  }

  g_autoptr(JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body, (gssize) length, &error)) {
    gtk_label_set_text (GTK_LABEL (runtime->search_info), "Search returned invalid JSON.");
    g_bytes_unref (bytes);
    goto done;
  }

  JsonNode *root_node = json_parser_get_root (parser);
  JsonObject *root = JSON_NODE_HOLDS_OBJECT (root_node) ? json_node_get_object (root_node) : NULL;
  JsonObject *tracks = root && json_object_has_member (root, "tracks") ?
                       json_object_get_object_member (root, "tracks") : NULL;
  JsonArray *items = tracks && json_object_has_member (tracks, "items") ?
                     json_object_get_array_member (tracks, "items") : NULL;
  guint found = 0;
  if (items) {
    for (guint i = 0; i < json_array_get_length (items); i++) {
      JsonObject *item = json_array_get_object_element (items, i);
      if (!item) continue;
      const gchar *uri = json_object_get_string_member_with_default (item, "uri", NULL);
      const gchar *name = json_object_get_string_member_with_default (item, "name", "Untitled track");
      if (!uri || !g_str_has_prefix (uri, "spotify:track:")) continue;

      g_autofree gchar *artist = NULL;
      JsonArray *artists = json_object_has_member (item, "artists") ?
                           json_object_get_array_member (item, "artists") : NULL;
      if (artists && json_array_get_length (artists) > 0) {
        JsonObject *artist_obj = json_array_get_object_element (artists, 0);
        if (artist_obj)
          artist = g_strdup (json_object_get_string_member_with_default (artist_obj, "name", "Unknown artist"));
      }
      g_autofree gchar *label = g_strdup_printf ("%s — %s", name,
                                                 artist ? artist : "Unknown artist");
      GtkWidget *row = gtk_button_new_with_label (label);
      gtk_widget_set_halign (row, GTK_ALIGN_FILL);
      gtk_widget_set_hexpand (row, TRUE);
      g_object_set_data_full (G_OBJECT (row), "track-uri", g_strdup (uri), g_free);
      g_signal_connect (row, "clicked", G_CALLBACK (on_search_result_clicked), runtime);
      gtk_list_box_append (GTK_LIST_BOX (runtime->search_results), row);
      found++;
    }
  }
  gtk_label_set_text (GTK_LABEL (runtime->search_info), found ?
                      "Select a result, then press Play." : "No tracks found.");
  g_bytes_unref (bytes);

done:
  catalog_search_closure_free (closure);
}

static void
start_catalog_search (GuiRuntime *runtime, const gchar *query)
{
  if (!runtime->search_auth || !native_auth_has_valid_token (runtime->search_auth)) {
    g_clear_object (&runtime->search_auth);
    runtime->search_auth = native_auth_new ();
  }
  if (!native_auth_has_valid_token (runtime->search_auth)) {
    gtk_label_set_text (GTK_LABEL (runtime->search_info),
                        "Catalog search needs a valid native login. Press Play once to authenticate.");
    return;
  }

  if (!runtime->search_session)
    runtime->search_session = soup_session_new_with_options ("user-agent", "SpotifyGTK/" APP_VERSION, NULL);
  if (runtime->search_cancellable) {
    g_cancellable_cancel (runtime->search_cancellable);
    g_clear_object (&runtime->search_cancellable);
  }
  runtime->search_cancellable = g_cancellable_new ();
  clear_search_results (runtime);
  gtk_label_set_text (GTK_LABEL (runtime->search_info), "Searching Spotify…");

  g_autofree gchar *encoded = g_uri_escape_string (query, NULL, FALSE);
  g_autofree gchar *url = g_strdup_printf (
      "https://api.spotify.com/v1/search?q=%s&type=track&limit=10", encoded);
  SoupMessage *message = soup_message_new (SOUP_METHOD_GET, url);
  g_autofree gchar *auth = g_strdup_printf ("Bearer %s",
                                            native_auth_get_token (runtime->search_auth));
  soup_message_headers_replace (soup_message_get_request_headers (message), "Authorization", auth);
  soup_message_headers_replace (soup_message_get_request_headers (message), "Accept", "application/json");
  CatalogSearchClosure *closure = g_new0 (CatalogSearchClosure, 1);
  g_weak_ref_init (&closure->window_ref, runtime->window);
  closure->message = g_object_ref (message);
  soup_session_send_and_read_async (runtime->search_session, message, G_PRIORITY_DEFAULT,
                                    runtime->search_cancellable,
                                    on_catalog_search_response, closure);
  g_object_unref (message);
}

static void
on_search_activated (GtkSearchEntry *entry, gpointer user_data)
{
  GuiRuntime *runtime = user_data;
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  g_autofree gchar *uri = NULL;

  if (g_str_has_prefix (text, "spotify:track:"))
    uri = g_strdup (text);
  else if (strlen (text) == 22) {
    gboolean base62 = TRUE;
    for (gsize i = 0; i < 22; i++)
      if (!g_ascii_isalnum (text[i])) base62 = FALSE;
    if (base62)
      uri = g_strdup_printf ("spotify:track:%s", text);
  }

  if (!uri) {
    start_catalog_search (runtime, text);
    return;
  }

  gtk_editable_set_text (GTK_EDITABLE (runtime->track_entry), uri);
  set_status_message (runtime, "Track selected. Press Play to start the native engine.");
  g_message ("gui: selected track from search: %s", uri);
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

  GtkWidget *results_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (results_box, 12);
  gtk_widget_set_margin_end (results_box, 12);
  gtk_widget_set_margin_top (results_box, 8);
  gtk_widget_set_margin_bottom (results_box, 8);
  runtime->search_info = gtk_label_new ("Search Spotify from the header to find tracks.");
  gtk_label_set_xalign (GTK_LABEL (runtime->search_info), 0.0f);
  gtk_widget_add_css_class (runtime->search_info, "dim-label");
  runtime->search_results = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (runtime->search_results), GTK_SELECTION_NONE);
  gtk_widget_set_vexpand (runtime->search_results, TRUE);
  gtk_box_append (GTK_BOX (results_box), runtime->search_info);
  gtk_box_append (GTK_BOX (results_box), runtime->search_results);
  gtk_box_append (GTK_BOX (page), section ("Search results", results_box));

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

  GtkWidget *header = gtk_header_bar_new ();
  GtkWidget *header_title = gtk_label_new ("SpotifyGTK Native");
  gtk_widget_add_css_class (header_title, "title");
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), header_title);
  gtk_window_set_titlebar (window, header);

  GtkWidget *stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  GuiRuntime *runtime = g_new0 (GuiRuntime, 1);
  runtime->window = window;
  runtime->player = spotifygtk_player_service_new ();
  g_signal_connect (runtime->player, "state-changed", G_CALLBACK (on_player_state_changed), runtime);
  GtkWidget *header_center = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *app_title = gtk_label_new ("SpotifyGTK Native");
  gtk_widget_add_css_class (app_title, "title");
  gtk_box_append (GTK_BOX (header_center), app_title);
  runtime->search_entry = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (runtime->search_entry),
                                         "Search tracks or paste a Spotify URI");
  gtk_widget_set_size_request (runtime->search_entry, 360, -1);
  gtk_widget_set_hexpand (runtime->search_entry, TRUE);
  g_signal_connect (runtime->search_entry, "activate",
                    G_CALLBACK (on_search_activated), runtime);
  gtk_box_append (GTK_BOX (header_center), runtime->search_entry);
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), header_center);
  gtk_stack_add_titled (GTK_STACK (stack), build_now_playing_page (runtime), "now-playing", "Now Playing");
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

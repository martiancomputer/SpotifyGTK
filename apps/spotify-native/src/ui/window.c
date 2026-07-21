/*
 * window.c — Main application window matching the mockup design.
 *
 * Dark theme layout:
 * ┌────────────────────────────────────────────────────────────────┐
 * │ Header Bar (SpotifyGTK)                                        │
 * ├──────────┬─────────────────────────────────┬──────────────────┤
 * │ Sidebar  │ Main Content                    │ Now Playing Panel│
 * │ (270px)  │                                 │ (300px)          │
 * │ Home     │ [Continue Listening cards]      │ Queue            │
 * │ Search   │                                 │ Track list       │
 * │ Liked    │ [Recently Played list]          │                  │
 * │ Library  │                                 │                  │
 * │ Downloads│                                 │                  │
 * │ ──────── │                                 │                  │
 * │ Pinned   │                                 │                  │
 * └──────────┴─────────────────────────────────┴──────────────────┘
 * │ Playback Bar (80px)                                            │
 * └────────────────────────────────────────────────────────────────┘
 */

#include "window.h"
#include "sidebar.h"
#include "playback_bar.h"
#include "now_playing_panel.h"
#include "home_page.h"
#include "search_page.h"
#include "liked_songs_page.h"
#include "library_page.h"

#include "../player_service.h"
#include "../spotify/session.h"

struct _SpotifyGtkNativeWindow {
  GtkApplicationWindow parent_instance;

  /* Layout widgets */
  GtkPaned *main_paned;       /* Sidebar | Content+Queue */
  GtkPaned *content_paned;    /* Main content | Queue panel */
  GtkBox *root_box;

  /* Sidebar */
  SpotifyGtkSidebar *sidebar;

  /* Pages */
  GtkStack *page_stack;
  SpotifyGtkHomePage *home_page;
  SpotifyGtkSearchPage *search_page;
  SpotifyGtkLikedSongsPage *liked_page;
  SpotifyGtkLibraryPage *library_page;

  /* Now Playing panel (right side) */
  SpotifyGtkNowPlayingPanel *now_playing_panel;

  /* Playback bar (bottom) */
  SpotifyGtkPlaybackBar *playback_bar;

  /* Core services */
  SpotifyNativePlayerService *player;
  SpotifyNativeSession *session;

  /* State */
  gchar *current_track_uri;
  gboolean queue_expanded;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkNativeWindow, spotifygtk_native_window, GTK_TYPE_APPLICATION_WINDOW)

/* === Forward declarations === */
static void navigate_to_page (SpotifyGtkNativeWindow *self, const gchar *page_name);

/* === Track selection === */

/* Adopt a track from any page: record its URI, update both display surfaces,
 * and hand it to the engine. Everything that starts playback goes through
 * here so current_track_uri can never drift from what's on screen. */
static void
select_and_play_track (SpotifyGtkNativeWindow *self, const SpotifyNativeTrack *track)
{
  if (!track || !track->uri) {
    g_warning ("Track has no URI; nothing to play");
    return;
  }

  g_free (self->current_track_uri);
  self->current_track_uri = g_strdup (track->uri);

  const gchar *name    = track->name    ? track->name    : "Unknown track";
  const gchar *artists = track->artists ? track->artists : "";
  const gchar *album   = track->album   ? track->album   : "";

  spotifygtk_playback_bar_set_track (self->playback_bar, name, artists);
  spotifygtk_now_playing_panel_set_track (self->now_playing_panel, name, artists, album);

  GError *error = NULL;
  if (!spotifygtk_player_service_start_uri (self->player, track->uri, &error)) {
    g_warning ("Playback failed: %s", error->message);
    g_error_free (error);
  }
}

static void
on_page_track_activated (gpointer page, gpointer track, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  select_and_play_track (self, track);
  (void) page;
}

/* === Sidebar callbacks === */
static void
on_sidebar_page_activated (SpotifyGtkSidebar *sidebar,
                           const gchar *page_id,
                           gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  navigate_to_page (self, page_id);
  (void) sidebar;
}

/* === Playback bar callbacks === */
static void
on_play_clicked (SpotifyGtkPlaybackBar *bar, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;

  /* Resuming a paused stream is not the same as starting one: restarting the
   * URI would re-fetch from byte zero and lose the buffered position. */
  if (spotifygtk_player_service_is_paused (self->player)) {
    spotifygtk_player_service_resume (self->player);
    (void) bar;
    return;
  }

  if (!self->current_track_uri) {
    g_message ("No track selected");
    return;
  }

  GError *error = NULL;
  if (!spotifygtk_player_service_start_uri (self->player, self->current_track_uri, &error)) {
    g_warning ("Playback failed: %s", error->message);
    g_error_free (error);
  }
  (void) bar;
}

static void
on_volume_changed (SpotifyGtkPlaybackBar *bar, gint percent, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  spotifygtk_player_service_set_volume (self->player, percent);
  (void) bar;
}

static void
on_pause_clicked (SpotifyGtkPlaybackBar *bar, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  spotifygtk_player_service_pause (self->player);
  (void) bar;
}

static void
on_next_clicked (SpotifyGtkPlaybackBar *bar, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  /* TODO: Queue next track */
  (void) self; (void) bar;
}

static void
on_prev_clicked (SpotifyGtkPlaybackBar *bar, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  /* TODO: Queue previous track */
  (void) self; (void) bar;
}

/* === Player state === */
static void
on_player_state_changed (SpotifyNativePlayerService *player,
                         gint state,
                         const gchar *message,
                         gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;

  gboolean is_playing = (state == SPOTIFYGTK_PLAYER_PLAYING);
  spotifygtk_playback_bar_set_playing (self->playback_bar, is_playing);

  if (state == SPOTIFYGTK_PLAYER_ERROR) {
    g_warning ("Player error: %s", message);
  }

  (void) player;
}

/* === Session === */
static void
on_session_state_changed (SpotifyNativeSession *session, gint state,
                          const gchar *message, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;

  g_message ("session: %s", message ? message : "");

  if (state == SPOTIFYGTK_SESSION_READY) {
    /* Pages that gave up with "Not signed in yet" can load for real now. */
    spotifygtk_search_page_set_session (self->search_page, session);
    spotifygtk_liked_songs_page_set_session (self->liked_page, session);

    const gchar *visible = gtk_stack_get_visible_child_name (self->page_stack);
    if (visible)
      navigate_to_page (self, visible);
  }
}

/* === Navigation === */
static void
navigate_to_page (SpotifyGtkNativeWindow *self, const gchar *page_name)
{
  if (!gtk_stack_get_child_by_name (self->page_stack, page_name)) {
    g_warning ("No page named '%s'", page_name);
    return;
  }

  gtk_stack_set_visible_child_name (self->page_stack, page_name);

  /* Load on first visit rather than at startup; the page no-ops the
   * refresh once it holds data. Home and Library are static for now. */
  if (g_strcmp0 (page_name, "liked") == 0)
    spotifygtk_liked_songs_page_refresh (self->liked_page);
}

/* === CSS for dark theme === */
static const gchar *dark_theme_css =
  "window { background-color: #1b1b1b; }"
  ".header-bar { background-color: #262626; }"
  ".sidebar { background-color: #232323; }"
  ".sidebar-item { padding: 12px 20px; border-radius: 10px; }"
  ".sidebar-item:hover { background-color: #2a2a2a; }"
  ".sidebar-item:selected { background-color: #343434; }"
  ".main-content { background-color: #1d1d1d; }"
  ".now-playing-panel { background-color: #202225; }"
  ".playback-bar { background-color: #171717; }"
  ".card { background-color: #2b2b2b; border-radius: 12px; }"
  ".list-row { background-color: #2b2b2b; border-radius: 8px; margin: 4px 0; }"
  ".list-row:hover { background-color: #333333; }"
  ".title-text { color: #ffffff; font-weight: 700; font-size: 34px; }"
  ".normal-text { color: #ececec; font-size: 16px; }"
  ".dim-text { color: #9f9f9f; font-size: 13px; }"
  ".pinned-card { background-color: #2b2b2b; border-radius: 8px; }";

static void
load_dark_theme (void)
{
  static gboolean loaded = FALSE;
  if (loaded) return;

  GtkCssProvider *provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider, dark_theme_css);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
  loaded = TRUE;
}

/* === Construction === */
static void
spotifygtk_native_window_constructed (GObject *object)
{
  SpotifyGtkNativeWindow *self = SPOTIFYGTK_NATIVE_WINDOW (object);
  G_OBJECT_CLASS (spotifygtk_native_window_parent_class)->constructed (object);

  load_dark_theme ();

  gtk_window_set_title (GTK_WINDOW (self), "SpotifyGTK");
  gtk_window_set_default_size (GTK_WINDOW (self), 1800, 900);

  /* Create core services */
  self->player = spotifygtk_player_service_new ();
  self->session = spotifygtk_native_session_new ();

  g_signal_connect (self->player, "state-changed",
                    G_CALLBACK (on_player_state_changed), self);
  g_signal_connect (self->session, "state-changed",
                    G_CALLBACK (on_session_state_changed), self);

  /* Root vertical box: header + content + playback bar */
  self->root_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_widget_set_vexpand (GTK_WIDGET (self->root_box), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->root_box), TRUE);

  /* Header bar */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (header, -1, 44);
  gtk_widget_add_css_class (header, "header-bar");

  GtkWidget *title = gtk_label_new ("SpotifyGTK");
  gtk_widget_add_css_class (title, "normal-text");
  gtk_widget_set_halign (title, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_box_append (GTK_BOX (header), title);

  gtk_box_append (GTK_BOX (self->root_box), header);

  /* Horizontal paned: sidebar | (content | queue) */
  self->main_paned = GTK_PANED (gtk_paned_new (GTK_ORIENTATION_HORIZONTAL));

  /* Sidebar (270px) */
  self->sidebar = spotifygtk_sidebar_new ();
  gtk_widget_set_size_request (GTK_WIDGET (self->sidebar), 270, -1);
  gtk_widget_add_css_class (GTK_WIDGET (self->sidebar), "sidebar");
  g_signal_connect (self->sidebar, "page-activated",
                    G_CALLBACK (on_sidebar_page_activated), self);
  gtk_paned_set_start_child (self->main_paned, GTK_WIDGET (self->sidebar));
  gtk_paned_set_resize_start_child (self->main_paned, FALSE);
  gtk_paned_set_shrink_start_child (self->main_paned, FALSE);

  /* Content + Queue paned */
  self->content_paned = GTK_PANED (gtk_paned_new (GTK_ORIENTATION_HORIZONTAL));

  /* Main content area */
  GtkWidget *content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (content_box, TRUE);
  gtk_widget_set_vexpand (content_box, TRUE);
  gtk_widget_add_css_class (content_box, "main-content");

  self->page_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->page_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->page_stack), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->page_stack), TRUE);

  self->home_page = spotifygtk_home_page_new ();
  self->search_page = spotifygtk_search_page_new ();
  self->liked_page = spotifygtk_liked_songs_page_new ();
  self->library_page = spotifygtk_library_page_new ();

  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->home_page), "home");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->search_page), "search");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->liked_page), "liked");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->library_page), "library");

  /* Search and Liked Songs are the two pages backed by the native session;
   * Home and Library are static until their endpoints are ported. */
  g_signal_connect (self->search_page, "track-activated",
                    G_CALLBACK (on_page_track_activated), self);
  g_signal_connect (self->liked_page, "track-activated",
                    G_CALLBACK (on_page_track_activated), self);

  gtk_box_append (GTK_BOX (content_box), GTK_WIDGET (self->page_stack));
  gtk_paned_set_start_child (self->content_paned, content_box);
  gtk_paned_set_resize_start_child (self->content_paned, TRUE);
  gtk_paned_set_shrink_start_child (self->content_paned, FALSE);

  /* Now Playing panel (300px) */
  self->now_playing_panel = spotifygtk_now_playing_panel_new ();
  gtk_widget_set_size_request (GTK_WIDGET (self->now_playing_panel), 300, -1);
  gtk_widget_add_css_class (GTK_WIDGET (self->now_playing_panel), "now-playing-panel");
  gtk_paned_set_end_child (self->content_paned, GTK_WIDGET (self->now_playing_panel));
  gtk_paned_set_resize_end_child (self->content_paned, FALSE);
  gtk_paned_set_shrink_end_child (self->content_paned, FALSE);

  gtk_paned_set_end_child (self->main_paned, GTK_WIDGET (self->content_paned));
  gtk_paned_set_resize_end_child (self->main_paned, TRUE);
  gtk_paned_set_shrink_end_child (self->main_paned, FALSE);

  /* Set positions */
  gtk_paned_set_position (self->main_paned, 270);
  gtk_paned_set_position (self->content_paned, 1230);

  gtk_widget_set_vexpand (GTK_WIDGET (self->main_paned), TRUE);
  gtk_box_append (GTK_BOX (self->root_box), GTK_WIDGET (self->main_paned));

  /* Playback bar (80px) */
  self->playback_bar = spotifygtk_playback_bar_new ();
  gtk_widget_set_size_request (GTK_WIDGET (self->playback_bar), -1, 80);
  gtk_widget_add_css_class (GTK_WIDGET (self->playback_bar), "playback-bar");
  g_signal_connect (self->playback_bar, "play-clicked",
                    G_CALLBACK (on_play_clicked), self);
  g_signal_connect (self->playback_bar, "pause-clicked",
                    G_CALLBACK (on_pause_clicked), self);
  g_signal_connect (self->playback_bar, "next-clicked",
                    G_CALLBACK (on_next_clicked), self);
  g_signal_connect (self->playback_bar, "prev-clicked",
                    G_CALLBACK (on_prev_clicked), self);
  g_signal_connect (self->playback_bar, "volume-changed",
                    G_CALLBACK (on_volume_changed), self);
  gtk_box_append (GTK_BOX (self->root_box), GTK_WIDGET (self->playback_bar));

  gtk_window_set_child (GTK_WINDOW (self), GTK_WIDGET (self->root_box));

  self->queue_expanded = TRUE;

  /* Sign in. Everything protocol-related happens on the session's worker
   * thread; "state-changed" arrives back here on the GTK thread. */
  spotifygtk_native_session_start (self->session);

  /* SPOTIFY_START_PAGE opens straight onto a given page, so a specific one
   * can be exercised without clicking through the UI. Unknown names fall
   * back to Home via navigate_to_page's own guard. */
  const gchar *start_page = g_getenv ("SPOTIFY_START_PAGE");
  navigate_to_page (self, (start_page && *start_page) ? start_page : "home");
}

static void
spotifygtk_native_window_dispose (GObject *object)
{
  SpotifyGtkNativeWindow *self = SPOTIFYGTK_NATIVE_WINDOW (object);

  g_clear_object (&self->player);
  g_clear_object (&self->session);
  g_clear_pointer (&self->current_track_uri, g_free);

  G_OBJECT_CLASS (spotifygtk_native_window_parent_class)->dispose (object);
}

static void
spotifygtk_native_window_class_init (SpotifyGtkNativeWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->constructed = spotifygtk_native_window_constructed;
  object_class->dispose = spotifygtk_native_window_dispose;
}

static void
spotifygtk_native_window_init (SpotifyGtkNativeWindow *self)
{
}

SpotifyGtkNativeWindow *
spotifygtk_native_window_new (GtkApplication *app)
{
  return g_object_new (SPOTIFYGTK_TYPE_NATIVE_WINDOW,
                       "application", app,
                       NULL);
}

void
spotifygtk_native_window_navigate_to (SpotifyGtkNativeWindow *self,
                                      const gchar *page_name)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_WINDOW (self));
  navigate_to_page (self, page_name);
}

void
spotifygtk_native_window_set_playing (SpotifyGtkNativeWindow *self,
                                      gboolean is_playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_WINDOW (self));
  spotifygtk_playback_bar_set_playing (self->playback_bar, is_playing);
  spotifygtk_now_playing_panel_set_playing (self->now_playing_panel, is_playing);
}

void
spotifygtk_native_window_set_track_info (SpotifyGtkNativeWindow *self,
                                         const gchar *track_name,
                                         const gchar *artist,
                                         const gchar *album,
                                         const gchar *album_art_url)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_WINDOW (self));

  /* Display-only: the selected URI is owned by select_and_play_track and must
   * not be touched here. (This used to g_free it without clearing, leaving a
   * dangling pointer for the next play click and dispose to double-free.) */
  spotifygtk_playback_bar_set_track (self->playback_bar, track_name, artist);
  spotifygtk_now_playing_panel_set_track (self->now_playing_panel,
                                          track_name, artist, album);
  spotifygtk_now_playing_panel_set_album_art (self->now_playing_panel, album_art_url);
  (void) album_art_url;
}

void
spotifygtk_native_window_set_progress (SpotifyGtkNativeWindow *self,
                                       gint64 position_ms,
                                       gint64 duration_ms)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_WINDOW (self));
  spotifygtk_playback_bar_set_progress (self->playback_bar, position_ms, duration_ms);
  spotifygtk_now_playing_panel_set_progress (self->now_playing_panel, position_ms, duration_ms);
}

void
spotifygtk_native_window_set_queue_expanded (SpotifyGtkNativeWindow *self,
                                             gboolean expanded)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_WINDOW (self));

  if (self->queue_expanded == expanded)
    return;

  self->queue_expanded = expanded;
  gtk_widget_set_visible (GTK_WIDGET (self->now_playing_panel), expanded);
}

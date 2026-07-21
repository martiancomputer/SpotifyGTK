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
#include "settings_page.h"

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
  SpotifyGtkSettingsPage *settings_page;

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

  spotifygtk_playback_bar_set_cover (self->playback_bar, track->cover_id);
  spotifygtk_now_playing_panel_set_cover (self->now_playing_panel, track->cover_id);

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
on_queue_clicked (SpotifyGtkPlaybackBar *bar, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  spotifygtk_native_window_set_queue_expanded (self, !self->queue_expanded);
  (void) bar;
}

/* Reachable from two places on purpose: the sidebar's own Collapse action,
 * and the header's menu button. The Collapse action disappears with the
 * sidebar, so without an entry point outside it a collapsed sidebar could
 * never be recovered. */
static void
on_sidebar_collapse_toggled (gpointer source, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  gboolean visible = gtk_widget_get_visible (GTK_WIDGET (self->sidebar));
  gtk_widget_set_visible (GTK_WIDGET (self->sidebar), !visible);
  (void) source;
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
  gboolean has_track   = (state == SPOTIFYGTK_PLAYER_PLAYING ||
                          state == SPOTIFYGTK_PLAYER_PAUSED ||
                          state == SPOTIFYGTK_PLAYER_BUFFERING ||
                          state == SPOTIFYGTK_PLAYER_CONNECTING);

  spotifygtk_playback_bar_set_playing (self->playback_bar, is_playing);

  /* Tell every list which row is current, so the equaliser follows the track
   * across pages rather than only appearing on the one that started it. */
  const gchar *uri = has_track ? self->current_track_uri : NULL;
  spotifygtk_search_page_set_playing_uri (self->search_page, uri, is_playing);
  spotifygtk_liked_songs_page_set_playing_uri (self->liked_page, uri, is_playing);

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

/* The Now Playing panel is bounded in both directions. Below the minimum the
 * cover and track text stop being legible; above the maximum the cover keeps
 * growing until it dominates the window, which is the "logarithmic" feel of
 * dragging it wide. GtkPaned exposes no max-position, so the clamp is applied
 * whenever the position changes. */
#define NOW_PLAYING_MIN_WIDTH 280
#define NOW_PLAYING_MAX_WIDTH 420

static void
on_content_paned_position (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  GtkPaned *paned = GTK_PANED (object);
  SpotifyGtkNativeWindow *self = user_data;

  gint total = gtk_widget_get_width (GTK_WIDGET (paned));
  if (total <= 0)
    return;

  gint position = gtk_paned_get_position (paned);
  gint panel_width = total - position;
  gint clamped = CLAMP (panel_width, NOW_PLAYING_MIN_WIDTH, NOW_PLAYING_MAX_WIDTH);

  if (clamped != panel_width)
    gtk_paned_set_position (paned, total - clamped);

  (void) pspec; (void) self;
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

  /* A page that has just (re)built its rows does not know what is playing. */
  gboolean is_playing =
    spotifygtk_player_service_get_state (self->player) == SPOTIFYGTK_PLAYER_PLAYING;
  spotifygtk_search_page_set_playing_uri (self->search_page,
                                          self->current_track_uri, is_playing);
  spotifygtk_liked_songs_page_set_playing_uri (self->liked_page,
                                               self->current_track_uri, is_playing);
}

/* === CSS for dark theme === */
/*
 * Styling follows the reference design: near-black chrome, a slightly
 * lifted content area, and a green accent used only for state (selection,
 * progress, pinned, active toggles) rather than as decoration.
 *
 *   #0a0a0a  window chrome / playback bar (deepest)
 *   #0f0f0f  sidebar
 *   #121212  main content
 *   #1a1a1a  cards and rows
 *   #242424  hover
 *   #1db954  accent
 */
static const gchar *dark_theme_css =
  "window { background-color: #0a0a0a; color: #e8e8e8; }"

  /* ── Structure ─────────────────────────────────────────────── */
  "headerbar { background-color: #0a0a0a; box-shadow: none;"
  "  border-bottom: 1px solid #000000; min-height: 46px; }"
  "headerbar label.title { font-size: 14px; font-weight: 600; color: #e8e8e8; }"
  ".sidebar { background-color: #0f0f0f;"
  "  border-right: 1px solid #000000; }"
  ".main-content { background-color: #121212; }"
  /* libadwaita paints list, row, viewport and .view with a lighter "view"
   * fill. That is the grey slab that showed through wherever a list was
   * empty. Nothing in this UI wants a filled list surface. */
  "list, list > row, scrolledwindow, scrolledwindow > viewport, viewport, .view"
  "  { background-color: transparent; background-image: none; }"
  "scrolledwindow undershoot.top, scrolledwindow undershoot.bottom"
  "  { background: none; }"
  ".now-playing-panel { background-color: #0f0f0f;"
  "  border-left: 1px solid #000000; }"
  /* Padding, not widget margins — see playback_bar.c. Bottom corners are
   * rounded to follow the window's own shape instead of squaring off
   * against it. */
  ".playback-bar { background-color: #0a0a0a;"
  "  border-top: 1px solid #000000;"
  "  padding: 6px 16px 8px 16px;"
  "  border-bottom-left-radius: 12px;"
  "  border-bottom-right-radius: 12px; }"

  /* ── Sidebar navigation ────────────────────────────────────── */
  ".sidebar list { background-color: transparent; }"
  ".sidebar-item { border-radius: 10px; margin: 2px 10px;"
  "  transition: background-color 120ms ease; }"
  ".sidebar-item label { color: #b8b8b8; font-size: 15px; }"
  ".sidebar-item image { color: #b8b8b8; }"
  ".sidebar-item:hover { background-color: #1a1a1a; }"
  ".sidebar-item:selected { background-color: #1f1f1f; }"
  ".sidebar-item:selected label { color: #ffffff; font-weight: 600; }"
  ".sidebar-item:selected image { color: #1db954; }"
  ".sidebar-heading { color: #7a7a7a; font-size: 12px; font-weight: 700;"
  "  letter-spacing: 0.6px; }"
  ".pinned-card { background-color: transparent; border-radius: 8px;"
  "  margin: 1px 10px; }"
  ".pinned-card:hover { background-color: #1a1a1a; }"
  ".pin-icon { color: #1db954; }"
  ".sidebar-action { color: #9a9a9a; font-size: 13px; }"

  /* ── Typography ────────────────────────────────────────────── */
  ".title-text { color: #ffffff; font-weight: 800; font-size: 30px;"
  "  letter-spacing: -0.5px; }"
  ".greeting { color: #9a9a9a; font-size: 14px; }"
  ".section-heading { color: #ffffff; font-size: 19px; font-weight: 700; }"
  ".normal-text { color: #e8e8e8; font-size: 15px; }"
  ".dim-text { color: #9a9a9a; font-size: 13px; }"
  ".bar-title { color: #ffffff; font-size: 14px; font-weight: 600; }"
  ".bar-subtitle { color: #9a9a9a; font-size: 12px; }"
  ".time-label { color: #9a9a9a; font-size: 11px;"
  "  font-feature-settings: \'tnum\'; }"
  ".row-number { color: #7a7a7a; font-size: 13px;"
  "  font-feature-settings: \'tnum\'; }"
  ".row-duration { color: #9a9a9a; font-size: 13px;"
  "  font-feature-settings: \'tnum\'; }"

  /* ── Cards and rows ────────────────────────────────────────── */
  ".card { background-color: #1a1a1a; border-radius: 10px; }"
  ".media-card { background-color: #1a1a1a; border-radius: 10px;"
  "  padding: 0px; transition: background-color 140ms ease; }"
  ".media-card:hover { background-color: #242424; }"
  ".media-card-title { color: #ffffff; font-size: 14px; font-weight: 600; }"
  ".media-card-subtitle { color: #9a9a9a; font-size: 12px; }"
  /* Track rows are a continuous list, not stacked cards. Giving each row a
   * solid fill plus a vertical margin banded the whole page. Flat with a
   * hover highlight; `.card` stays for things that really are cards. */
  ".list-row { background-color: transparent; border-radius: 6px;"
  "  transition: background-color 120ms ease; }"
  ".list-row:hover { background-color: #1c1c1c; }"
  ".list-row:selected { background-color: #232323; }"
  ".art-thumb { background-color: #151515; border-radius: 6px;"
  "  color: #3e3e3e; }"
  ".art-large { background-color: #151515; border-radius: 12px;"
  "  color: #333333; }"
  ".pill-button { background-color: #1f1f1f; border-radius: 999px;"
  "  color: #e8e8e8; font-size: 12px; font-weight: 600;"
  "  padding: 4px 14px; min-height: 0; }"
  ".pill-button:hover { background-color: #2c2c2c; }"

  /* ── Transport ─────────────────────────────────────────────── */
  /* min-width/height must match the widget size request, or GTK button
   * padding wins and the "circular" class renders an oval. */
  ".play-button { background-color: #ffffff; color: #0a0a0a;"
  "  min-width: 44px; min-height: 44px; padding: 0; }"
  ".play-button:hover { background-color: #f0f0f0; }"
  ".play-button:disabled { background-color: #3a3a3a; color: #7a7a7a; }"
  ".transport-button { color: #b8b8b8; min-width: 32px; min-height: 32px; }"
  ".transport-button:hover { color: #ffffff; }"
  ".toggle-active { color: #1db954; }"
  ".like-active { color: #1db954; }"

  /* ── Sliders ───────────────────────────────────────────────── */
  "scale { min-height: 18px; }"
  "scale trough { background-color: #3a3a3a; min-height: 4px;"
  "  border-radius: 2px; }"
  "scale highlight { background-color: #1db954; border-radius: 2px; }"
  "scale:disabled highlight { background-color: #4a4a4a; }"
  /* `margin: 0` is load-bearing: libadwaita puts a negative margin on scale
   * sliders, which combines with a smaller min-width to give a negative
   * computed size and a stream of GTK warnings. */
  "scale slider { background-color: #ffffff; min-width: 12px;"
  "  min-height: 12px; border-radius: 6px; margin: 0; }"
  "scale:disabled slider { background-color: #6e6e6e; }"

  /* ── Text selection ────────────────────────────────────────── */
  /* One selection colour everywhere: the search entry, and any selectable
   * label in results, playlists, liked songs or albums. */
  /* GTK4 models selection as a `selection` node, not the CSS ::selection
   * pseudo-element -- including the latter makes the whole rule fail to
   * parse ("Unknown pseudoclass"). */
  "selection, entry selection, label selection, text selection"
  "  { background-color: #60A5FA; color: #0a0a0a; }"
  "entry { caret-color: #60A5FA; }"

  /* ── Now playing indicator ─────────────────────────────────── */
  /* Three bars beside the duration on whichever row is playing. The
   * animation is driven in C (track_row.c) rather than by a CSS keyframe,
   * so the tick can be stopped outright when no row is playing instead of
   * leaving the compositor animating an offscreen widget forever. */
  ".eq-bar { background-color: #1db954; border-radius: 1px; }"

  /* ── Scrollbars ────────────────────────────────────────────── */
  /* Non-overlay, so it sits in a gutter beside the list, not over it. */
  "scrollbar { background-color: transparent; border: none; }"
  "scrollbar slider { background-color: #3a3a3a; border-radius: 6px;"
  "  min-width: 8px; margin: 2px; }"
  "scrollbar slider:hover { background-color: #4e4e4e; }";

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

  /* Header bar. A real GtkHeaderBar rather than a plain box, so the window
   * controls and drag-to-move come from GTK instead of being drawn on. */
  GtkWidget *header = gtk_header_bar_new ();
  gtk_header_bar_set_show_title_buttons (GTK_HEADER_BAR (header), TRUE);

  GtkWidget *menu_btn = gtk_button_new_from_icon_name ("open-menu-symbolic");
  gtk_widget_add_css_class (menu_btn, "flat");
  gtk_widget_set_tooltip_text (menu_btn, "Show or hide the sidebar");
  g_signal_connect (menu_btn, "clicked",
                    G_CALLBACK (on_sidebar_collapse_toggled), self);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), menu_btn);

  GtkWidget *title = gtk_label_new ("SpotifyGTK");
  gtk_widget_add_css_class (title, "title");
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), title);

  gtk_window_set_titlebar (GTK_WINDOW (self), header);

  /* Horizontal paned: sidebar | (content | queue) */
  self->main_paned = GTK_PANED (gtk_paned_new (GTK_ORIENTATION_HORIZONTAL));

  /* Sidebar (270px) */
  self->sidebar = spotifygtk_sidebar_new ();
  gtk_widget_set_size_request (GTK_WIDGET (self->sidebar), 270, -1);
  gtk_widget_add_css_class (GTK_WIDGET (self->sidebar), "sidebar");
  g_signal_connect (self->sidebar, "page-activated",
                    G_CALLBACK (on_sidebar_page_activated), self);
  g_signal_connect (self->sidebar, "collapse-toggled",
                    G_CALLBACK (on_sidebar_collapse_toggled), self);
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
  self->settings_page = spotifygtk_settings_page_new ();

  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->home_page), "home");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->search_page), "search");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->liked_page), "liked");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->library_page), "library");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->settings_page), "settings");

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

  g_signal_connect (self->content_paned, "notify::position",
                    G_CALLBACK (on_content_paned_position), self);

  /* The content/panel divider is deliberately not given a fixed position.
   * A hardcoded pixel value only lines up at one window width; at any other
   * it leaves a dead strip between the track list and the Now Playing
   * panel. With resize_end FALSE the panel takes its own size request and
   * the content absorbs the rest, at every width. */

  gtk_widget_set_vexpand (GTK_WIDGET (self->main_paned), TRUE);
  gtk_box_append (GTK_BOX (self->root_box), GTK_WIDGET (self->main_paned));

  /* Playback bar (80px) */
  self->playback_bar = spotifygtk_playback_bar_new ();
  /* No fixed height. The bar gained a second row (progress under the
   * transport buttons), and an 80px request clipped it against the bottom
   * of the window -- it now sizes to its content. */
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
  g_signal_connect (self->playback_bar, "queue-clicked",
                    G_CALLBACK (on_queue_clicked), self);
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
spotifygtk_native_window_navigate_to_settings (SpotifyGtkNativeWindow *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_WINDOW (self));
  navigate_to_page (self, "settings");
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

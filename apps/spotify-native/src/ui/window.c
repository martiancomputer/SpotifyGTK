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
#include "spotify/native_auth.h"
#include "settings.h"
#include "context_page.h"
#include "track_list.h"

#include "../player_service.h"
#include "../spotify/session.h"

#include <string.h>

struct _SpotifyGtkNativeWindow {
  GtkApplicationWindow parent_instance;

  /* Layout widgets */
  GtkPaned *main_paned;       /* Sidebar | Content+Queue */
  GtkPaned *content_paned;    /* Main content | Queue panel */
  GtkBox *root_box;

  /* Sidebar */
  SpotifyGtkSidebar *sidebar;

  /* Login gate: an opaque layer over the whole window that stays up until the
   * session actually reaches READY. */
  GtkWidget  *login_gate;
  GtkWidget  *menu_btn;        /* app header controls, disabled while gated */
  GtkWidget  *login_button;
  GtkLabel   *login_status;
  NativeAuth *auth;

  /* Header navigation, browser-style. `nav_history` holds NavEntry* in visit
   * order; `nav_pos` indexes the current one, so Back/Forward just step it. */
  GtkWidget *nav_back;
  GtkWidget *nav_fwd;
  GPtrArray *nav_history;
  gint       nav_pos;

  /* Pages */
  GtkStack *page_stack;
  SpotifyGtkHomePage *home_page;
  SpotifyGtkSearchPage *search_page;
  SpotifyGtkLikedSongsPage *liked_page;
  SpotifyGtkLibraryPage *library_page;
  SpotifyGtkSettingsPage *settings_page;
  SpotifyGtkContextPage *context_page;   /* album / artist target */

  /* Now Playing panel (right side) */
  SpotifyGtkNowPlayingPanel *now_playing_panel;

  /* Playback bar (bottom) */
  SpotifyGtkPlaybackBar *playback_bar;

  /* Core services */
  SpotifyNativePlayerService *player;
  SpotifyNativeSession *session;

  /* State */
  gchar *current_track_uri;
  gint64 current_track_duration_ms;   /* for the progress bar; engine reports position only */
  gboolean queue_expanded;

  /* Playback ordering. `play_context` is the ordered list the current track
   * came from (a snapshot of whichever page was activated); `context_index`
   * points at the current track within it. `user_queue` holds tracks the
   * user explicitly queued, which play before the context advances. All hold
   * owned SpotifyNativeTrack copies. The engine still plays one track at a
   * time — this layer only decides which URI to hand it next. */
  GPtrArray *play_context;      /* SpotifyNativeTrack*, free func set; may be NULL */
  gint       context_index;     /* -1 when there is no context */
  GQueue    *user_queue;        /* SpotifyNativeTrack*, freed manually */
};

G_DEFINE_FINAL_TYPE (SpotifyGtkNativeWindow, spotifygtk_native_window, GTK_TYPE_APPLICATION_WINDOW)

/* === Forward declarations === */
static void spotifygtk_native_window_show_login_gate (SpotifyGtkNativeWindow *self,
                                                      const gchar *status);
static void navigate_to_page (SpotifyGtkNativeWindow *self, const gchar *page_name);
static void navigate_raw (SpotifyGtkNativeWindow *self, const gchar *page_name);
static void navigate_to_context (SpotifyGtkNativeWindow *self, const gchar *uri,
                                 const gchar *title, const gchar *kind);
static void spotifygtk_native_window_collapse_queue (SpotifyGtkNativeWindow *self);

/* === Header navigation history === */

typedef struct {
  gchar *page;    /* stack child name */
  gchar *uri;     /* context pages only; NULL for plain pages */
  gchar *title;
  gchar *kind;
} NavEntry;

static void
nav_entry_free (gpointer data)
{
  NavEntry *e = data;
  g_free (e->page); g_free (e->uri); g_free (e->title); g_free (e->kind);
  g_free (e);
}

static void
nav_update_buttons (SpotifyGtkNativeWindow *self)
{
  if (self->nav_back)
    gtk_widget_set_sensitive (self->nav_back, self->nav_pos > 0);
  if (self->nav_fwd)
    gtk_widget_set_sensitive (self->nav_fwd,
                              self->nav_pos >= 0 &&
                              self->nav_pos < (gint) self->nav_history->len - 1);
}

/* Record a freshly-reached location, dropping any forward history (a new move
 * forks the timeline) and collapsing a repeat of the current spot. */
static void
nav_record (SpotifyGtkNativeWindow *self, const gchar *page,
            const gchar *uri, const gchar *title, const gchar *kind)
{
  while ((gint) self->nav_history->len > self->nav_pos + 1)
    g_ptr_array_remove_index (self->nav_history, self->nav_history->len - 1);

  if (self->nav_history->len > 0) {
    NavEntry *top = g_ptr_array_index (self->nav_history, self->nav_history->len - 1);
    if (g_strcmp0 (top->page, page) == 0 && g_strcmp0 (top->uri, uri) == 0)
      return;
  }

  NavEntry *e = g_new0 (NavEntry, 1);
  e->page = g_strdup (page); e->uri = g_strdup (uri);
  e->title = g_strdup (title); e->kind = g_strdup (kind);
  g_ptr_array_add (self->nav_history, e);
  self->nav_pos = (gint) self->nav_history->len - 1;
  nav_update_buttons (self);
}

/* Step Back (-1) or Forward (+1) without recording -- replaying history. */
static void
nav_go (SpotifyGtkNativeWindow *self, gint dir)
{
  gint target = self->nav_pos + dir;
  if (target < 0 || target >= (gint) self->nav_history->len)
    return;

  self->nav_pos = target;
  NavEntry *e = g_ptr_array_index (self->nav_history, target);
  if (e->uri)
    spotifygtk_context_page_load (self->context_page, e->uri,
                                  e->title ? e->title : "Album",
                                  e->kind ? e->kind : "Album");
  navigate_raw (self, e->page);
  nav_update_buttons (self);
}

static void
on_nav_back_clicked (GtkButton *b, gpointer user_data)
{
  nav_go (user_data, -1);
  (void) b;
}

static void
on_nav_fwd_clicked (GtkButton *b, gpointer user_data)
{
  nav_go (user_data, +1);
  (void) b;
}

/* === Track selection, queue and play-context === */

/* Rebuild the "up next" list the panel shows, and update Prev/Next
 * sensitivity, from the current queue and context. Called whenever either
 * changes. The panel list borrows the track pointers, so it must be rebuilt
 * (not retained) any time the underlying arrays are mutated. */
static void
refresh_transport_and_queue (SpotifyGtkNativeWindow *self)
{
  gboolean can_prev = self->play_context && self->context_index > 0;
  gboolean can_next = !g_queue_is_empty (self->user_queue) ||
                      (self->play_context && self->context_index >= 0 &&
                       self->context_index + 1 < (gint) self->play_context->len);
  spotifygtk_playback_bar_set_skip_sensitive (self->playback_bar, can_prev, can_next);

  /* Up next = user-queued tracks first, then the tail of the context, capped
   * to a small window. The Now Playing queue is a plain GtkListBox (one
   * widget per row, not virtualised), and playing track 1 of a 500-track
   * context used to dump ~499 rows into it — each fetching a cover — which
   * stuttered or froze the moment a track started. A queue nobody scrolls
   * past a few dozen entries has no need to show the whole tail. */
  #define UP_NEXT_MAX 40
  GPtrArray *up_next = g_ptr_array_new ();   /* borrowed pointers, no free func */
  for (GList *l = self->user_queue->head; l && up_next->len < UP_NEXT_MAX; l = l->next)
    g_ptr_array_add (up_next, l->data);
  if (self->play_context && self->context_index >= 0) {
    for (guint i = self->context_index + 1;
         i < self->play_context->len && up_next->len < UP_NEXT_MAX; i++)
      g_ptr_array_add (up_next, g_ptr_array_index (self->play_context, i));
  }
  spotifygtk_now_playing_panel_set_native_queue (self->now_playing_panel, up_next);
  g_ptr_array_free (up_next, TRUE);
}

/* Show a track on both surfaces and hand its URI to the engine. This is the
 * one place that touches the engine and current_track_uri, so they can never
 * drift from what's on screen. It does not touch the queue or context —
 * callers own that decision. */
static void
play_native_track (SpotifyGtkNativeWindow *self, const SpotifyNativeTrack *track)
{
  if (!track || !track->uri) {
    g_warning ("Track has no URI; nothing to play");
    return;
  }

  g_free (self->current_track_uri);
  self->current_track_uri = g_strdup (track->uri);
  self->current_track_duration_ms = track->duration_ms;

  /* Reset the bar to 0 of the new track's length immediately; the engine's
   * position reports then advance it. */
  spotifygtk_native_window_set_progress (self, 0, track->duration_ms);

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

  refresh_transport_and_queue (self);
}

/* Play the context entry at `index`, making it the current track. */
static void
play_context_at (SpotifyGtkNativeWindow *self, gint index)
{
  if (!self->play_context || index < 0 || index >= (gint) self->play_context->len)
    return;
  self->context_index = index;
  play_native_track (self, g_ptr_array_index (self->play_context, index));
}

/* Resolve and play the next track: a user-queued track first, otherwise the
 * next context entry. Returns TRUE if something started. A queued track plays
 * without disturbing the context cursor, so the context resumes after it. */
static gboolean
advance_next (SpotifyGtkNativeWindow *self)
{
  if (!g_queue_is_empty (self->user_queue)) {
    SpotifyNativeTrack *track = g_queue_pop_head (self->user_queue);
    play_native_track (self, track);
    spotifygtk_native_track_free (track);
    return TRUE;
  }

  if (self->play_context && self->context_index >= 0 &&
      self->context_index + 1 < (gint) self->play_context->len) {
    play_context_at (self, self->context_index + 1);
    return TRUE;
  }

  return FALSE;
}

static gboolean
advance_prev (SpotifyGtkNativeWindow *self)
{
  if (self->play_context && self->context_index > 0) {
    play_context_at (self, self->context_index - 1);
    return TRUE;
  }
  return FALSE;
}

/* A row was activated on `list`: adopt that list's ordering as the new play
 * context and play the chosen track from within it, so Next/Previous walk the
 * same list the user is looking at. */
static void
on_list_track_activated (SpotifyGtkTrackList *list, gpointer track_ptr, gpointer user_data)
{
  SpotifyGtkNativeWindow   *self  = user_data;
  const SpotifyNativeTrack *track = track_ptr;
  if (!track || !track->uri)
    return;

  g_clear_pointer (&self->play_context, g_ptr_array_unref);
  self->play_context = spotifygtk_track_list_snapshot (list);
  self->context_index = -1;

  for (guint i = 0; i < self->play_context->len; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (self->play_context, i);
    if (g_strcmp0 (t->uri, track->uri) == 0) {
      self->context_index = (gint) i;
      break;
    }
  }

  if (self->context_index >= 0)
    play_context_at (self, self->context_index);
  else
    play_native_track (self, track);   /* not found in snapshot; play it alone */
}

static void
on_list_add_to_queue (SpotifyGtkTrackList *list, gpointer track_ptr, gpointer user_data)
{
  SpotifyGtkNativeWindow   *self  = user_data;
  const SpotifyNativeTrack *track = track_ptr;
  if (!track || !track->uri)
    return;

  g_queue_push_tail (self->user_queue, spotifygtk_native_track_copy (track));
  refresh_transport_and_queue (self);
  (void) list;
}

static void
on_list_go_to_album (SpotifyGtkTrackList *list, gpointer track_ptr, gpointer user_data)
{
  SpotifyGtkNativeWindow   *self  = user_data;
  const SpotifyNativeTrack *track = track_ptr;
  if (!track || !track->album_uri)
    return;

  navigate_to_context (self, track->album_uri,
                       track->album ? track->album : "Album", "Album");
  (void) list;
}

static void
on_list_go_to_artist (SpotifyGtkTrackList *list, gpointer track_ptr, gpointer user_data)
{
  SpotifyGtkNativeWindow   *self  = user_data;
  const SpotifyNativeTrack *track = track_ptr;
  if (!track || !track->artist_uri)
    return;

  /* `artists` is ", "-joined; show just the primary for the header. */
  g_autofree gchar *primary = NULL;
  if (track->artists && *track->artists) {
    const gchar *sep = strstr (track->artists, ", ");
    primary = sep ? g_strndup (track->artists, sep - track->artists)
                  : g_strdup (track->artists);
  }

  navigate_to_context (self, track->artist_uri,
                       primary ? primary : "Artist", "Artist");
  (void) list;
}

/* An album card was clicked, on any page's shelf or grid. Same destination as
 * the row menu's "Go to Album": the album URI handed to the context page. */
static void
on_album_activated (SpotifyGtkAlbumGrid *grid, const gchar *uri,
                    const gchar *name, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  if (!uri || !*uri)
    return;
  navigate_to_context (self, uri, name && *name ? name : "Album", "Album");
  (void) grid;
}

static void
wire_album_grid (SpotifyGtkNativeWindow *self, SpotifyGtkAlbumGrid *grid)
{
  if (!grid)
    return;
  g_signal_connect (grid, "album-activated", G_CALLBACK (on_album_activated), self);
}

/* Connect the window to one page's inner list: activation drives play +
 * context, and the row context menu drives queue and album/artist nav. Every
 * page that shows tracks routes through the same handlers. */
static void
wire_track_list (SpotifyGtkNativeWindow *self, SpotifyGtkTrackList *list)
{
  if (!list)
    return;
  g_signal_connect (list, "track-activated", G_CALLBACK (on_list_track_activated), self);
  g_signal_connect (list, "add-to-queue",    G_CALLBACK (on_list_add_to_queue),    self);
  g_signal_connect (list, "go-to-album",     G_CALLBACK (on_list_go_to_album),     self);
  g_signal_connect (list, "go-to-artist",    G_CALLBACK (on_list_go_to_artist),    self);
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
on_seek_requested (SpotifyGtkPlaybackBar *bar, gint64 position_ms, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  spotifygtk_player_service_seek (self->player, position_ms);
  (void) bar;
}

static void
on_next_clicked (SpotifyGtkPlaybackBar *bar, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  advance_next (self);
  (void) bar;
}

static void
on_prev_clicked (SpotifyGtkPlaybackBar *bar, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  advance_prev (self);
  (void) bar;
}

/* === Player state === */
static void
on_player_state_changed (SpotifyNativePlayerService *player,
                         gint state,
                         const gchar *message,
                         gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;

  /* A track finishing on its own reaches the window as IDLE (a user switching
   * tracks never does — the service hands the pending URI straight to the
   * engine without emitting IDLE). So IDLE with a track loaded means "play
   * whatever is next". If nothing is next, fall through and clear the UI. */
  if (state == SPOTIFYGTK_PLAYER_IDLE && self->current_track_uri) {
    if (advance_next (self))
      return;
  }

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
  spotifygtk_context_page_set_playing_uri (self->context_page, uri, is_playing);

  if (state == SPOTIFYGTK_PLAYER_ERROR) {
    g_warning ("Player error: %s", message);
  }

  (void) player;
}

static void
on_player_position_changed (SpotifyNativePlayerService *player,
                            gint64 position_ms, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  spotifygtk_native_window_set_progress (self, position_ms,
                                         self->current_track_duration_ms);
  (void) player;
}

/* === Login gate ===
 *
 * An opaque layer over the entire window, shown whenever there is no usable
 * session, and taken down only once the session actually reports READY —
 * "a token exists" is not the same as "the token works", and signing in is the
 * one thing that must not be half-shown. It is an overlay rather than a
 * separate page so nothing behind it can be clicked or scrolled while it is up,
 * A GtkWindow's titlebar sits outside the window child, so an overlay cannot
 * cover it. The app's own header controls (menu, Back/Forward) are therefore
 * disabled alongside the gate -- but NOT the GtkHeaderBar itself, which also
 * owns the window's close/minimize/maximize buttons: desensitising the whole
 * bar left no way to close or resize the window while signed out.
 *
 * Styled from the same @-colours as everything else — @bg_content ground,
 * @accent for the button — rather than a login-specific palette.
 */
static void
on_auth_completed (NativeAuth *auth, gboolean success, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;

  gtk_widget_set_sensitive (self->login_button, TRUE);

  if (!success) {
    gtk_label_set_text (self->login_status,
                        "Sign-in was not completed. Try again.");
    return;
  }

  /* The token is stored; the session picks it up on start. The gate stays up
   * until state-changed reports READY. */
  gtk_label_set_text (self->login_status, "Signing in…");
  spotifygtk_native_session_start (self->session);
  (void) auth;
}

static void
on_login_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;

  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  gtk_label_set_text (self->login_status,
                      "Waiting for you to approve access in your browser…");
  native_auth_begin (self->auth);
}

static void
spotifygtk_native_window_show_login_gate (SpotifyGtkNativeWindow *self,
                                          const gchar *status)
{
  if (!self->login_gate)
    return;

  gtk_widget_set_sensitive (self->login_button, TRUE);
  gtk_label_set_text (self->login_status, status ? status : "");
  gtk_widget_set_visible (self->login_gate, TRUE);

  /* Window controls stay live; only navigation into a signed-out UI is barred. */
  if (self->menu_btn) gtk_widget_set_sensitive (self->menu_btn, FALSE);
  if (self->nav_back) gtk_widget_set_sensitive (self->nav_back, FALSE);
  if (self->nav_fwd)  gtk_widget_set_sensitive (self->nav_fwd,  FALSE);
}

static GtkWidget *
build_login_gate (SpotifyGtkNativeWindow *self)
{
  /* Fills the overlay and paints the ground opaque, so the UI behind it is
   * neither visible nor reachable. */
  GtkWidget *gate = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (gate, "login-gate");
  gtk_widget_set_hexpand (gate, TRUE);
  gtk_widget_set_vexpand (gate, TRUE);
  gtk_widget_set_halign (gate, GTK_ALIGN_FILL);
  gtk_widget_set_valign (gate, GTK_ALIGN_FILL);

  GtkWidget *centre = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_halign (centre, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (centre, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (centre, TRUE);

  GtkWidget *title = gtk_label_new ("SpotifyGTK");
  gtk_widget_add_css_class (title, "title-text");
  gtk_box_append (GTK_BOX (centre), title);

  GtkWidget *blurb = gtk_label_new (
    "Sign in with your Spotify account to load your library and play music.");
  gtk_widget_add_css_class (blurb, "dim-text");
  gtk_label_set_justify (GTK_LABEL (blurb), GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap (GTK_LABEL (blurb), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (blurb), 44);
  gtk_box_append (GTK_BOX (centre), blurb);

  self->login_button = gtk_button_new_with_label ("Log into SpotifyGTK");
  gtk_widget_add_css_class (self->login_button, "login-button");
  gtk_widget_set_halign (self->login_button, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (self->login_button, 8);
  g_signal_connect (self->login_button, "clicked",
                    G_CALLBACK (on_login_clicked), self);
  gtk_box_append (GTK_BOX (centre), self->login_button);

  self->login_status = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->login_status), "dim-text");
  gtk_label_set_justify (self->login_status, GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap (self->login_status, TRUE);
  gtk_label_set_max_width_chars (self->login_status, 44);
  gtk_box_append (GTK_BOX (centre), GTK_WIDGET (self->login_status));

  gtk_box_append (GTK_BOX (gate), centre);
  return gate;
}

void
spotifygtk_native_window_log_out (SpotifyGtkNativeWindow *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_WINDOW (self));

  /* Stop playback first: the engine holds an AP session minted from the
   * credentials we are about to discard. */
  if (self->player)
    spotifygtk_player_service_stop (self->player);

  native_auth_log_out (self->auth);

  spotifygtk_native_window_show_login_gate (self, "Signed out.");
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
    spotifygtk_context_page_set_session (self->context_page, session);
    spotifygtk_home_page_set_session (self->home_page, session);
    spotifygtk_library_page_set_session (self->library_page, session);

    const gchar *visible = gtk_stack_get_visible_child_name (self->page_stack);
    if (visible)
      navigate_raw (self, visible);   /* refresh in place; not a history move */

    /* Only now is the sign-in real, so this is the one place the gate lifts. */
    if (self->login_gate)
      gtk_widget_set_visible (self->login_gate, FALSE);
    if (self->menu_btn)
      gtk_widget_set_sensitive (self->menu_btn, TRUE);
    nav_update_buttons (self);   /* re-enable per actual history position */
  } else if (state == SPOTIFYGTK_SESSION_FAILED) {
    /* A stored token that the server refuses is indistinguishable, from here,
     * from having no token at all — put the gate back so there is a way out
     * other than restarting. */
    spotifygtk_native_window_show_login_gate (
      self, message && *message ? message : "Could not sign in.");
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
navigate_raw (SpotifyGtkNativeWindow *self, const gchar *page_name)
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
  spotifygtk_context_page_set_playing_uri (self->context_page,
                                           self->current_track_uri, is_playing);
}

/* Plain-page navigation that records history (sidebar clicks, etc.). */
static void
navigate_to_page (SpotifyGtkNativeWindow *self, const gchar *page_name)
{
  navigate_raw (self, page_name);
  nav_record (self, page_name, NULL, NULL, NULL);
}

/* Open an album/artist context page and record it, so Back returns here. */
static void
navigate_to_context (SpotifyGtkNativeWindow *self, const gchar *uri,
                     const gchar *title, const gchar *kind)
{
  spotifygtk_context_page_load (self->context_page, uri, title, kind);
  navigate_raw (self, "context");
  nav_record (self, "context", uri, title, kind);
}

/* === Theming ===================================================== */
/*
 * The reference design is one dark stylesheet; White and Milk are the same
 * rules over a different palette. To stop the three from drifting, the rule
 * body below is written once against named colours (@bg_chrome, @accent, …)
 * and each theme supplies only the palette that defines them — so a rule
 * added or tweaked once takes effect in all three.
 *
 * Named rather than literal because several roles share a hex in one theme
 * but invert in another: the play button and slider knob are near-white on
 * dark chrome but near-black on a light one, so @ctrl_fill cannot just be
 * "#ffffff". The Dark palette reproduces the exact values the design shipped
 * with (the one deliberate exception: the .art-large placeholder glyph moves
 * from #333333 to #3e3e3e, sharing @art_glyph with .art-thumb — an invisible
 * shift on a barely-visible glyph).
 *
 *   @bg_chrome  window chrome / playback bar (deepest)
 *   @bg_panel   sidebar / now-playing panel
 *   @bg_content main content
 *   @bg_card    cards and rows
 *   @accent     green, used only for state
 */

/* The shared rule body. Every colour is an @-name resolved by the palette
 * prepended at load time. */
static const gchar *theme_body =
  "window { background-color: @bg_chrome; color: @fg; }"

  /* ── Structure ─────────────────────────────────────────────── */
  "headerbar { background-color: @bg_chrome; box-shadow: none;"
  "  border-bottom: 1px solid @border; min-height: 46px; }"
  "headerbar label.title { font-size: 14px; font-weight: 600; color: @fg; }"
  ".sidebar { background-color: @bg_panel;"
  "  border-right: 1px solid @border; }"
  ".main-content { background-color: @bg_content; }"
  /* Login gate: opaque so nothing shows through, and drawn from the same
   * palette as the rest of the UI rather than a sign-in-specific one. */
  ".login-gate { background-color: @bg_content; }"
  ".login-button { background-color: @accent; color: @on_accent;"
  "  font-size: 15px; font-weight: 700; border-radius: 999px;"
  "  padding: 12px 34px; min-height: 0;"
  "  transition: background-color 140ms ease; }"
  ".login-button:hover { background-color: @ctrl_fill_hover; color: @ctrl_on_fill; }"
  ".login-button:disabled { background-color: @bg_selected; color: @fg_dim; }"
  /* Frosted header on the search page. The rows scroll underneath it (see the
   * GtkOverlay in search_page.c); this gradient is what sells that -- solid at
   * the top where the title and entry sit, then fading to fully transparent at
   * its lower edge so a row sliding up dissolves into the page instead of being
   * chopped by a hard line. GTK has no backdrop blur, so a soft gradient fade
   * over the same @bg_content is the honest approximation of frosted glass. */
  ".search-glass { background-image: linear-gradient(to bottom,"
  "  @bg_content 0%, @bg_content 60%,"
  "  alpha(@bg_content, 0.75) 82%, alpha(@bg_content, 0.0) 100%); }"
  /* libadwaita paints list, row, viewport and .view with a lighter "view"
   * fill. That is the grey slab that showed through wherever a list was
   * empty. Nothing in this UI wants a filled list surface. */
  "list, list > row, scrolledwindow, scrolledwindow > viewport, viewport, .view"
  "  { background-color: transparent; background-image: none; }"
  "scrolledwindow undershoot.top, scrolledwindow undershoot.bottom"
  "  { background: none; }"
  ".now-playing-panel { background-color: @bg_panel;"
  "  border-left: 1px solid @border; }"
  /* Padding, not widget margins — see playback_bar.c. Bottom corners are
   * rounded to follow the window's own shape instead of squaring off
   * against it. */
  ".playback-bar { background-color: @bg_chrome;"
  "  border-top: 1px solid @border;"
  "  padding: 6px 16px 8px 16px;"
  "  border-bottom-left-radius: 12px;"
  "  border-bottom-right-radius: 12px; }"

  /* ── Sidebar navigation ────────────────────────────────────── */
  ".sidebar list { background-color: transparent; }"
  ".sidebar-item { border-radius: 10px; margin: 2px 10px;"
  "  transition: background-color 120ms ease; }"
  ".sidebar-item label { color: @fg_sidebar; font-size: 15px; }"
  ".sidebar-item image { color: @fg_sidebar; }"
  ".sidebar-item:hover { background-color: @bg_card; }"
  ".sidebar-item:selected { background-color: @bg_selected; }"
  ".sidebar-item:selected label { color: @fg_strong; font-weight: 600; }"
  ".sidebar-item:selected image { color: @accent; }"
  ".sidebar-heading { color: @fg_dimmer; font-size: 12px; font-weight: 700;"
  "  letter-spacing: 0.6px; }"
  ".pinned-card { background-color: transparent; border-radius: 8px;"
  "  margin: 1px 10px; }"
  ".pinned-card:hover { background-color: @bg_card; }"
  ".pin-icon { color: @accent; }"
  ".sidebar-action { color: @fg_dim; font-size: 13px; }"

  /* ── Typography ────────────────────────────────────────────── */
  ".title-text { color: @fg_strong; font-weight: 800; font-size: 30px;"
  "  letter-spacing: -0.5px; }"
  ".greeting { color: @fg_dim; font-size: 14px; }"
  ".section-heading { color: @fg_strong; font-size: 19px; font-weight: 700; }"
  ".normal-text { color: @fg; font-size: 15px; }"
  ".dim-text { color: @fg_dim; font-size: 13px; }"
  ".bar-title { color: @fg_strong; font-size: 14px; font-weight: 600; }"
  ".bar-subtitle { color: @fg_dim; font-size: 12px; }"
  ".time-label { color: @fg_dim; font-size: 11px;"
  "  font-feature-settings: \'tnum\'; }"
  ".row-number { color: @fg_dimmer; font-size: 13px;"
  "  font-feature-settings: \'tnum\'; }"
  ".row-duration { color: @fg_dim; font-size: 13px;"
  "  font-feature-settings: \'tnum\'; }"

  /* ── Cards and rows ────────────────────────────────────────── */
  ".card { background-color: @bg_card; border-radius: 10px; }"
  ".media-card { background-color: @bg_card; border-radius: 10px;"
  "  padding: 0px; transition: background-color 140ms ease; }"
  ".media-card:hover { background-color: @bg_hover; }"
  ".media-card-title { color: @fg_strong; font-size: 14px; font-weight: 600; }"
  ".media-card-subtitle { color: @fg_dim; font-size: 12px; }"
  /* Track rows are a continuous list, not stacked cards. Giving each row a
   * solid fill plus a vertical margin banded the whole page. Flat with a
   * hover highlight; `.card` stays for things that really are cards. */
  ".list-row { background-color: transparent; border-radius: 6px;"
  "  transition: background-color 120ms ease; }"
  ".list-row:hover { background-color: @bg_hover_row; }"
  ".list-row:selected { background-color: @bg_selected_row; }"
  ".art-thumb { background-color: @art_bg; border-radius: 6px;"
  "  color: @art_glyph; }"
  ".art-large { background-color: @art_bg; border-radius: 12px;"
  "  color: @art_glyph; }"
  ".pill-button { background-color: @bg_selected; border-radius: 999px;"
  "  color: @fg; font-size: 12px; font-weight: 600;"
  "  padding: 4px 14px; min-height: 0; }"
  ".pill-button:hover { background-color: @pill_hover; }"

  /* ── Transport ─────────────────────────────────────────────── */
  /* min-width/height must match the widget size request, or GTK button
   * padding wins and the "circular" class renders an oval. */
  ".play-button { background-color: @ctrl_fill; color: @ctrl_on_fill;"
  "  min-width: 34px; min-height: 34px; padding: 0; }"
  ".play-button:hover { background-color: @ctrl_fill_hover; }"
  ".play-button:disabled { background-color: @trough; color: @fg_dimmer; }"
  ".transport-button { color: @fg_sidebar; min-width: 32px; min-height: 32px; }"
  ".transport-button:hover { color: @fg_strong; }"
  ".toggle-active { color: @accent; }"
  ".like-active { color: @accent; }"

  /* ── Sliders ───────────────────────────────────────────────── */
  "scale { min-height: 18px; }"
  "scale trough { background-color: @trough; min-height: 4px;"
  "  border-radius: 2px; }"
  "scale highlight { background-color: @accent; border-radius: 2px; }"
  "scale:disabled highlight { background-color: @trough_muted; }"
  /* `margin: 0` is load-bearing: libadwaita puts a negative margin on scale
   * sliders, which combines with a smaller min-width to give a negative
   * computed size and a stream of GTK warnings. */
  "scale slider { background-color: @knob; min-width: 12px;"
  "  min-height: 12px; border-radius: 6px; margin: 0; }"
  "scale:disabled slider { background-color: @knob_muted; }"

  /* ── Text selection ────────────────────────────────────────── */
  /* One selection colour everywhere: the search entry, and any selectable
   * label in results, playlists, liked songs or albums. */
  /* GTK4 models selection as a `selection` node, not the CSS ::selection
   * pseudo-element -- including the latter makes the whole rule fail to
   * parse ("Unknown pseudoclass"). */
  "selection, entry selection, label selection, text selection"
  "  { background-color: @selection; color: @on_accent; }"
  "entry { caret-color: @selection; }"

  /* ── Now playing indicator ─────────────────────────────────── */
  /* Three bars beside the duration on whichever row is playing. The
   * animation is driven in C (track_row.c) rather than by a CSS keyframe,
   * so the tick can be stopped outright when no row is playing instead of
   * leaving the compositor animating an offscreen widget forever.
   * NOTE: the eq bars are also drawn in Cairo with a hardcoded green in
   * track_row.c; a non-green accent would want updating there too. */
  ".eq-bar { background-color: @accent; border-radius: 1px; }"

  /* ── Popovers / context menus ──────────────────────────────── */
  /* libadwaita draws its own popover background from the adw color scheme,
   * which did not match our surfaces — a right-click menu in dark mode came
   * up noticeably lighter than everything around it. Pin the popover chrome
   * and its menu rows to the palette so it matches. */
  "popover > contents, popover > arrow"
  "  { background-color: @bg_card; color: @fg;"
  "    border: 1px solid @border; box-shadow: 0 4px 16px rgba(0,0,0,0.5); }"
  "popover contents { padding: 4px; border-radius: 10px; }"
  "popover modelbutton, popover button.model"
  "  { color: @fg; border-radius: 6px; padding: 6px 10px; }"
  "popover modelbutton:hover, popover button.model:hover"
  "  { background-color: @bg_hover; }"
  "popover separator { background-color: @border; }"

  /* ── Scrollbars ────────────────────────────────────────────── */
  /* Non-overlay, so it sits in a gutter beside the list, not over it. */
  "scrollbar { background-color: transparent; border: none; }"
  "scrollbar slider { background-color: @trough; border-radius: 6px;"
  "  min-width: 8px; margin: 2px; }"
  "scrollbar slider:hover { background-color: @scroll_hover; }";

/* One @define-color block per theme. Dark is the shipped design, verbatim. */
static const gchar *palette_dark =
  "@define-color bg_chrome #0a0a0a;  @define-color bg_panel #0f0f0f;"
  "@define-color bg_content #121212; @define-color bg_card #1a1a1a;"
  "@define-color bg_hover #242424;   @define-color bg_hover_row #1c1c1c;"
  "@define-color bg_selected #1f1f1f;@define-color bg_selected_row #232323;"
  "@define-color border #000000;"
  "@define-color fg #e8e8e8;         @define-color fg_strong #ffffff;"
  "@define-color fg_dim #9a9a9a;     @define-color fg_dimmer #7a7a7a;"
  "@define-color fg_sidebar #b8b8b8;"
  "@define-color accent #1db954;     @define-color selection #60A5FA;"
  "@define-color on_accent #0a0a0a;"
  "@define-color ctrl_fill #ffffff;  @define-color ctrl_fill_hover #f0f0f0;"
  "@define-color ctrl_on_fill #0a0a0a;"
  "@define-color knob #ffffff;       @define-color knob_muted #6e6e6e;"
  "@define-color trough #3a3a3a;     @define-color trough_muted #4a4a4a;"
  "@define-color scroll_hover #4e4e4e;@define-color pill_hover #2c2c2c;"
  "@define-color art_bg #151515;     @define-color art_glyph #3e3e3e;";

/* Crisp white: dark ink on white, same green accent, a deeper blue selection
 * that stays legible on light. Play button / knob invert to near-black. */
static const gchar *palette_white =
  "@define-color bg_chrome #ffffff;  @define-color bg_panel #f6f6f6;"
  "@define-color bg_content #ffffff; @define-color bg_card #eeeeee;"
  "@define-color bg_hover #e6e6e6;   @define-color bg_hover_row #f0f0f0;"
  "@define-color bg_selected #e4e4e4;@define-color bg_selected_row #e4e4e4;"
  "@define-color border #e2e2e2;"
  "@define-color fg #202020;         @define-color fg_strong #0a0a0a;"
  "@define-color fg_dim #6a6a6a;     @define-color fg_dimmer #8a8a8a;"
  "@define-color fg_sidebar #565656;"
  "@define-color accent #1db954;     @define-color selection #2f6fed;"
  "@define-color on_accent #ffffff;"
  "@define-color ctrl_fill #0a0a0a;  @define-color ctrl_fill_hover #2a2a2a;"
  "@define-color ctrl_on_fill #ffffff;"
  "@define-color knob #0a0a0a;       @define-color knob_muted #b4b4b4;"
  "@define-color trough #d2d2d2;     @define-color trough_muted #dedede;"
  "@define-color scroll_hover #b8b8b8;@define-color pill_hover #dcdcdc;"
  "@define-color art_bg #e6e6e6;     @define-color art_glyph #bcbcbc;";

/* Milk: a warm off-white. Same structure as White, cream-shifted, with warm
 * near-black ink so it reads softer than the crisp White theme. */
static const gchar *palette_milk =
  "@define-color bg_chrome #f5f0e6;  @define-color bg_panel #efe9db;"
  "@define-color bg_content #faf6ee; @define-color bg_card #ece5d5;"
  "@define-color bg_hover #e6ddcc;   @define-color bg_hover_row #efe9db;"
  "@define-color bg_selected #e2d8c4;@define-color bg_selected_row #e2d8c4;"
  "@define-color border #e3daca;"
  "@define-color fg #34302a;         @define-color fg_strong #1e1b15;"
  "@define-color fg_dim #75695a;     @define-color fg_dimmer #9a8d78;"
  "@define-color fg_sidebar #6a5f4f;"
  "@define-color accent #1db954;     @define-color selection #2f6fed;"
  "@define-color on_accent #ffffff;"
  "@define-color ctrl_fill #2a241b;  @define-color ctrl_fill_hover #43392c;"
  "@define-color ctrl_on_fill #f5f0e6;"
  "@define-color knob #2a241b;       @define-color knob_muted #b3a88f;"
  "@define-color trough #d9ceb8;     @define-color trough_muted #e2d8c4;"
  "@define-color scroll_hover #bcae97;@define-color pill_hover #ded3bd;"
  "@define-color art_bg #e6ddcc;     @define-color art_glyph #bcae97;";

static const gchar *
palette_for (SpotifyGtkTheme theme)
{
  switch (theme) {
    case SPOTIFYGTK_THEME_LIGHT: return palette_white;
    case SPOTIFYGTK_THEME_MILK:  return palette_milk;
    case SPOTIFYGTK_THEME_DARK:
    default:                     return palette_dark;
  }
}

/*
 * Apply a theme by reloading one provider held on the display. It is created
 * once and its contents replaced on each switch, so a theme change restyles
 * the running window with no restart. AdwStyleManager is steered alongside so
 * libadwaita's own drawing (dropdown popups, entries, the context menu) picks
 * the matching light/dark base rather than fighting the palette.
 */
static void
apply_theme (SpotifyGtkTheme theme)
{
  static GtkCssProvider *provider = NULL;
  if (!provider) {
    provider = gtk_css_provider_new ();
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  g_autofree gchar *css = g_strconcat (palette_for (theme), theme_body, NULL);
  gtk_css_provider_load_from_string (provider, css);

  adw_style_manager_set_color_scheme (
    adw_style_manager_get_default (),
    theme == SPOTIFYGTK_THEME_DARK ? ADW_COLOR_SCHEME_FORCE_DARK
                                   : ADW_COLOR_SCHEME_FORCE_LIGHT);
}


/* Push the equaliser from settings into the player service. Called on startup
 * and whenever settings change, so a slider move applies to live audio. */
static void
apply_eq_from_settings (SpotifyGtkNativeWindow *self)
{
  SpotifyGtkSettings *s = spotifygtk_settings_get_default ();
  spotifygtk_player_service_set_eq (self->player,
                                    spotifygtk_settings_get_eq_gains (s),
                                    spotifygtk_settings_get_eq_enabled (s));

  spotifygtk_player_service_set_output_rate (
    self->player,
    spotifygtk_settings_sample_rate_hz (
      spotifygtk_settings_get_sample_rate (spotifygtk_settings_get_default ())));
}

static void
on_settings_theme_changed (SpotifyGtkSettings *settings, gpointer user_data)
{
  apply_theme (spotifygtk_settings_get_theme (settings));
  apply_eq_from_settings (user_data);
}

/* === Construction === */
static void
spotifygtk_native_window_constructed (GObject *object)
{
  SpotifyGtkNativeWindow *self = SPOTIFYGTK_NATIVE_WINDOW (object);
  G_OBJECT_CLASS (spotifygtk_native_window_parent_class)->constructed (object);

  /* Apply the persisted theme now, and re-apply whenever it changes — the
   * settings page writes through the same singleton, so a dropdown change
   * restyles the running window live. */
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  apply_theme (spotifygtk_settings_get_theme (settings));
  g_signal_connect (settings, "changed",
                    G_CALLBACK (on_settings_theme_changed), self);

  gtk_window_set_title (GTK_WINDOW (self), "SpotifyGTK");
  gtk_window_set_default_size (GTK_WINDOW (self), 1800, 900);

  /* Create core services */
  self->player = spotifygtk_player_service_new ();
  self->session = spotifygtk_native_session_new ();

  /* Seed the equaliser from the persisted settings now that the player
   * exists; later changes arrive via the settings "changed" handler. */
  apply_eq_from_settings (self);

  g_signal_connect (self->player, "state-changed",
                    G_CALLBACK (on_player_state_changed), self);
  g_signal_connect (self->player, "position-changed",
                    G_CALLBACK (on_player_position_changed), self);
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
  self->menu_btn = menu_btn;
  gtk_widget_add_css_class (menu_btn, "flat");
  gtk_widget_set_tooltip_text (menu_btn, "Show or hide the sidebar");
  g_signal_connect (menu_btn, "clicked",
                    G_CALLBACK (on_sidebar_collapse_toggled), self);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), menu_btn);

  /* Browser-style Back/Forward over the page history. */
  self->nav_back = gtk_button_new_from_icon_name ("go-previous-symbolic");
  gtk_widget_add_css_class (self->nav_back, "flat");
  gtk_widget_set_tooltip_text (self->nav_back, "Back");
  gtk_widget_set_sensitive (self->nav_back, FALSE);
  g_signal_connect (self->nav_back, "clicked", G_CALLBACK (on_nav_back_clicked), self);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), self->nav_back);

  self->nav_fwd = gtk_button_new_from_icon_name ("go-next-symbolic");
  gtk_widget_add_css_class (self->nav_fwd, "flat");
  gtk_widget_set_tooltip_text (self->nav_fwd, "Forward");
  gtk_widget_set_sensitive (self->nav_fwd, FALSE);
  g_signal_connect (self->nav_fwd, "clicked", G_CALLBACK (on_nav_fwd_clicked), self);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), self->nav_fwd);

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
  g_signal_connect_swapped (self->settings_page, "log-out",
                            G_CALLBACK (spotifygtk_native_window_log_out), self);
  self->context_page = spotifygtk_context_page_new ();

  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->home_page), "home");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->search_page), "search");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->liked_page), "liked");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->library_page), "library");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->settings_page), "settings");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->context_page), "context");

  /* Every track list — search results, liked songs, and an opened album or
   * artist — routes activation, queueing and album/artist navigation through
   * the window via the same handlers. Home and Library have no lists yet. */
  wire_track_list (self, spotifygtk_search_page_get_list (self->search_page));
  wire_track_list (self, spotifygtk_liked_songs_page_get_list (self->liked_page));
  wire_track_list (self, spotifygtk_context_page_get_list (self->context_page));

  wire_album_grid (self, spotifygtk_search_page_get_album_grid (self->search_page));
  wire_album_grid (self, spotifygtk_home_page_get_album_grid (self->home_page));
  wire_album_grid (self, spotifygtk_library_page_get_album_grid (self->library_page));

  gtk_box_append (GTK_BOX (content_box), GTK_WIDGET (self->page_stack));
  gtk_paned_set_start_child (self->content_paned, content_box);
  gtk_paned_set_resize_start_child (self->content_paned, TRUE);
  gtk_paned_set_shrink_start_child (self->content_paned, FALSE);

  /* Now Playing panel (300px) */
  self->now_playing_panel = spotifygtk_now_playing_panel_new ();
  g_signal_connect_swapped (self->now_playing_panel, "collapse-requested",
                            G_CALLBACK (spotifygtk_native_window_collapse_queue), self);
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
  g_signal_connect (self->playback_bar, "seek",
                    G_CALLBACK (on_seek_requested), self);
  g_signal_connect (self->playback_bar, "queue-clicked",
                    G_CALLBACK (on_queue_clicked), self);
  gtk_box_append (GTK_BOX (self->root_box), GTK_WIDGET (self->playback_bar));

  /* The whole UI sits under an overlay so the login gate can cover it. */
  GtkWidget *shell = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (shell), GTK_WIDGET (self->root_box));

  self->login_gate = build_login_gate (self);
  gtk_overlay_add_overlay (GTK_OVERLAY (shell), self->login_gate);

  gtk_window_set_child (GTK_WINDOW (self), shell);

  self->queue_expanded = TRUE;

  /* Sign in only if there is something to sign in with. Starting the session
   * without a token would send it down the AP path just to fail, so the gate
   * goes up first and the session starts when the user comes back from the
   * browser. Everything protocol-related happens on the session's worker
   * thread; "state-changed" arrives back here on the GTK thread. */
  self->auth = native_auth_new ();
  g_signal_connect (self->auth, "completed", G_CALLBACK (on_auth_completed), self);

  if (native_auth_has_valid_token (self->auth)) {
    gtk_widget_set_visible (self->login_gate, FALSE);
    spotifygtk_native_session_start (self->session);
  } else {
    spotifygtk_native_window_show_login_gate (self, NULL);
  }

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
  g_clear_pointer (&self->play_context, g_ptr_array_unref);
  g_clear_pointer (&self->nav_history, g_ptr_array_unref);
  g_clear_object (&self->auth);
  if (self->user_queue) {
    g_queue_free_full (self->user_queue, (GDestroyNotify) spotifygtk_native_track_free);
    self->user_queue = NULL;
  }

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
  self->context_index = -1;
  self->user_queue = g_queue_new ();
  self->nav_history = g_ptr_array_new_with_free_func (nav_entry_free);
  self->nav_pos = -1;
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

static void
spotifygtk_native_window_collapse_queue (SpotifyGtkNativeWindow *self)
{
  spotifygtk_native_window_set_queue_expanded (self, FALSE);
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

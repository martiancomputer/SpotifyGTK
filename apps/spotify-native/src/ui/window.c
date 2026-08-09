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
#include "cover_loader.h"
#include "sidebar.h"
#include "playback_bar.h"
#include "now_playing_panel.h"
#include "../audio/sink.h"
#include "home_page.h"
#include "search_page.h"
#include "liked_songs_page.h"
#include "library_page.h"
#include "settings_page.h"
#include "spotify/native_auth.h"
#include "settings.h"
#include "context_page.h"
#include "artist_page.h"
#include "track_list.h"

#include "../player_service.h"
#include "../native_engine.h"
#include "../log_verbose.h"
#include "../spotify/collection.h"
#include "../spotify/playlist.h"
#include "smooth_scroll.h"
#include "../spotify/protobuf_min.h"
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
  /* Every track list wired through wire_track_list(), so a liked mark can be
   * fanned out to all of them: one track can be on screen in more than one
   * list at a time. Borrowed -- the lists belong to their pages. */
  GPtrArray *track_lists;

  /* Every liked track URI, read at sign-in. The lists keep their own copies
   * for the rows they show; this is the authority. */
  GHashTable *liked_uris;

  /*
   * The set under construction while a read is in flight. The live set is only
   * replaced once the last page lands: clearing it up front left it empty for
   * the length of the read -- ten round trips on a large library -- during
   * which every heart read unliked and every menu offered "Add".
   */
  GHashTable *liked_building;
  gint64      last_local_write_us;

  /*
   * Likes made while the connection was down. The optimistic update already
   * happened, so dropping them would leave the UI asserting something the
   * server never heard -- which is what "it updated here but not on the web
   * player" was. Replayed when the session comes back.
   */
  GPtrArray *pending_likes;
  SpotifyGtkAlbumGrid *playlists_grid;   /* cards for the rootlist */
  guint playlists_generation;            /* stale in-flight card lookups */
  guint playlists_resolved;              /* cards that have asked for details */
  gboolean playlists_loading;            /* rootlist read in flight */
  guint collection_change_id;            /* coalesces change events */
  gboolean playlists_loaded;             /* the grid holds cards already */
  GtkWidget  *playlists_status;
  /* Tracks we have started, so a handover can render one we no longer hold. */
  GHashTable *display_tracks;
  /*
   * Set when the user picks a track, cleared when the sink reaches it.
   *
   * Between those two the outgoing track is still the audible one, so the
   * player service goes on reporting its position and can still announce it as
   * now-playing. Both would overwrite what the click already put on screen.
   */
  gchar      *awaiting_uri;
  guint64     collection_sub;   /* Mercury subscription for external changes */

  SpotifyGtkHomePage *home_page;
  SpotifyGtkSearchPage *search_page;
  SpotifyGtkLikedSongsPage *liked_page;
  SpotifyGtkLibraryPage *library_page;
  SpotifyGtkSettingsPage *settings_page;
  SpotifyGtkContextPage *context_page;   /* album / playlist target */
  SpotifyGtkArtistPage  *artist_page;    /* artists get their own layout */

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

  /*
   * The order the context is played in, as positions into play_context.
   *
   * Shuffle is a permutation held here rather than a reordering of
   * play_context itself: the list on screen must not rearrange under the user,
   * and turning shuffle off has to restore the real order, which is only
   * possible if it was never lost. context_index stays the position of the
   * current track in play_context, so everything that reads it is unaffected.
   */
  GArray    *order;             /* guint, indices into play_context */
  gint       order_pos;         /* cursor into order; -1 when none */
  gboolean   shuffle;
  SpotifyGtkRepeatMode repeat;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkNativeWindow, spotifygtk_native_window, GTK_TYPE_APPLICATION_WINDOW)

/* === Forward declarations === */
static void spotifygtk_native_window_show_login_gate (SpotifyGtkNativeWindow *self,
                                                      const gchar *status);
static void navigate_to_page (SpotifyGtkNativeWindow *self, const gchar *page_name);
static void spotifygtk_native_window_reload_liked (SpotifyGtkNativeWindow *self);
static void navigate_raw (SpotifyGtkNativeWindow *self, const gchar *page_name);
static gint order_pos_of (SpotifyGtkNativeWindow *self, gint ctx_index);
static void rebuild_order (SpotifyGtkNativeWindow *self, gint keep);
static void show_now_playing (SpotifyGtkNativeWindow *self, const SpotifyNativeTrack *track);
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
  if (e->uri) {
    /* Restoring a history entry has to pick the same page the entry was
     * recorded from, or Back would land an artist in the album view. */
    if (g_str_has_prefix (e->uri, "spotify:artist:"))
      spotifygtk_artist_page_show (self->artist_page, e->uri, e->title);
    else
      spotifygtk_context_page_load (self->context_page, e->uri,
                                    e->title ? e->title : "Album",
                                    e->kind ? e->kind : "Album");
  }
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
  gint opos = self->order_pos >= 0 ? self->order_pos
                                   : order_pos_of (self, self->context_index);
  gboolean can_prev = self->order && (opos > 0 ||
                      (self->repeat == SPOTIFYGTK_REPEAT_ALL && self->order->len > 0));
  gboolean can_next = !g_queue_is_empty (self->user_queue) ||
                      (self->order && opos >= 0 &&
                       (opos + 1 < (gint) self->order->len ||
                        self->repeat != SPOTIFYGTK_REPEAT_OFF));
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
  if (self->play_context && self->order && opos >= 0) {
    /* Walks the play order, so Up Next shows what shuffle will actually
     * play rather than whatever happens to sit below in the listing. */
    for (guint i = (guint) opos + 1;
         i < self->order->len && up_next->len < UP_NEXT_MAX; i++)
      g_ptr_array_add (up_next,
        g_ptr_array_index (self->play_context,
                           g_array_index (self->order, guint, i)));
  }
  spotifygtk_now_playing_panel_set_native_queue (self->now_playing_panel, up_next);
  g_ptr_array_free (up_next, TRUE);
}

/* Show a track on both surfaces and hand its URI to the engine. This is the
 * one place that touches the engine and current_track_uri, so they can never
 * drift from what's on screen. It does not touch the queue or context —
 * callers own that decision. */
static void
play_native_track (SpotifyGtkNativeWindow *self, const SpotifyNativeTrack *track,
                   gboolean handover)
{
  if (!track || !track->uri) {
    g_warning ("Track has no URI; nothing to play");
    return;
  }

  /*
   * Remember the track so the handover can render it later, then decide
   * whether "later" is now.
   *
   * Starting from silence there is nothing to wait for, so the bar updates on
   * the click as it always did. Starting while something is still sounding is
   * a gapless handover: the previous track has seconds of audio left, and
   * showing the new title over it would be a lie for those seconds. In that
   * case the render waits for now-playing-changed, which the player service
   * emits when the sink actually reaches it.
   */
  if (!self->display_tracks)
    self->display_tracks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                  (GDestroyNotify) spotifygtk_native_track_free);
  if (track->uri)
    g_hash_table_insert (self->display_tracks, g_strdup (track->uri),
                         spotifygtk_native_track_copy (track));

  /*
   * Render now unless this is a handover.
   *
   * Deferring exists for one case: a track ending into the next while several
   * seconds of its audio are still queued, where showing the new title early
   * would be a lie. Picking a track by hand is the opposite -- the one playing
   * is cancelled at once, so waiting for the sink to reach the new one left
   * the old song's cover, title and duration on screen through the gap, which
   * read as the previous track trying to play again.
   */
  if (!handover ||
      spotifygtk_audio_sink_current_seq (spotifygtk_audio_sink_get ()) == 0) {
    /*
     * Silence: adopt the track now, exactly as before.
     *
     * current_track_uri and the progress reset move with the render rather
     * than running ahead of it. They are not bookkeeping -- the URI is what
     * every list highlights as the playing row, and the reset zeroes the
     * progress bar. Doing either at the click would, during a handover, blank
     * the progress of a track still being heard and move the equaliser to a
     * row that is not sounding yet.
     */
    g_free (self->current_track_uri);
    self->current_track_uri = g_strdup (track->uri);
    self->current_track_duration_ms = track->duration_ms;
    spotifygtk_native_window_set_progress (self, 0, track->duration_ms);
    show_now_playing (self, track);

    /* Hold everything the outgoing track still has to say until the sink has
     * actually moved on. Only for a deliberate pick: a handover has no such
     * disagreement, the audible track is the one to believe. */
    if (handover) {
      g_clear_pointer (&self->awaiting_uri, g_free);
  g_clear_pointer (&self->order, g_array_unref);
    } else {
      g_free (self->awaiting_uri);
      self->awaiting_uri = g_strdup (track->uri);
    }
  }

  GError *error = NULL;
  if (!spotifygtk_player_service_start_uri (self->player, track->uri, &error)) {
    g_warning ("Playback failed: %s", error->message);
    g_error_free (error);
  }

  refresh_transport_and_queue (self);
}

/* Where `ctx_index` sits in the current play order, or -1. */
static gint
order_pos_of (SpotifyGtkNativeWindow *self, gint ctx_index)
{
  if (!self->order || ctx_index < 0)
    return -1;
  for (guint i = 0; i < self->order->len; i++)
    if ((gint) g_array_index (self->order, guint, i) == ctx_index)
      return (gint) i;
  return -1;
}

/*
 * Rebuild the play order for the current context.
 *
 * `keep` is the context position to treat as current; it is moved to the front
 * so that turning shuffle on does not change what is playing, only what comes
 * after it.
 */
static void
rebuild_order (SpotifyGtkNativeWindow *self, gint keep)
{
  g_clear_pointer (&self->order, g_array_unref);
  self->order_pos = -1;

  if (!self->play_context || self->play_context->len == 0)
    return;

  guint n = self->play_context->len;
  self->order = g_array_sized_new (FALSE, FALSE, sizeof (guint), n);
  for (guint i = 0; i < n; i++)
    g_array_append_val (self->order, i);

  if (self->shuffle) {
    /* Fisher-Yates, so every permutation is equally likely. Picking a random
     * track each time instead would repeat and strand tracks, which is what
     * people mean when they say a shuffle "isn't random". */
    for (guint i = n; i > 1; i--) {
      guint j = (guint) g_random_int_range (0, (gint32) i);
      guint tmp = g_array_index (self->order, guint, i - 1);
      g_array_index (self->order, guint, i - 1) = g_array_index (self->order, guint, j);
      g_array_index (self->order, guint, j) = tmp;
    }
  }

  if (keep >= 0) {
    gint at = order_pos_of (self, keep);
    if (at > 0) {
      guint tmp = g_array_index (self->order, guint, 0);
      g_array_index (self->order, guint, 0) = g_array_index (self->order, guint, at);
      g_array_index (self->order, guint, at) = tmp;
    }
    self->order_pos = 0;
  }
}

/* Play the context entry at `index`, making it the current track. */
static void
play_context_at (SpotifyGtkNativeWindow *self, gint index, gboolean handover)
{
  if (!self->play_context || index < 0 || index >= (gint) self->play_context->len)
    return;
  self->context_index = index;
  self->order_pos = order_pos_of (self, index);
  play_native_track (self, g_ptr_array_index (self->play_context, index), handover);
}

/* Resolve and play the next track: a user-queued track first, otherwise the
 * next context entry. Returns TRUE if something started. A queued track plays
 * without disturbing the context cursor, so the context resumes after it. */
/*
 * `handover` distinguishes a track ending into the next from a button press.
 * Only the first should hold the old title on screen while its remaining audio
 * plays out; a press stops the current track immediately, so the display must
 * follow immediately too.
 */
static gboolean
advance_next (SpotifyGtkNativeWindow *self, gboolean handover)
{
  if (!g_queue_is_empty (self->user_queue)) {
    SpotifyNativeTrack *track = g_queue_pop_head (self->user_queue);
    play_native_track (self, track, handover);
    spotifygtk_native_track_free (track);
    return TRUE;
  }

  if (!self->play_context || self->play_context->len == 0)
    return FALSE;

  /* Repeat-one replays the same entry, and only when the track ended on its
   * own -- pressing Next while it is on should still move on, or the button
   * would appear broken. */
  if (self->repeat == SPOTIFYGTK_REPEAT_ONE && handover && self->context_index >= 0) {
    play_context_at (self, self->context_index, handover);
    return TRUE;
  }

  if (!self->order)
    rebuild_order (self, self->context_index);
  if (!self->order || self->order->len == 0)
    return FALSE;

  gint pos = self->order_pos >= 0 ? self->order_pos
                                  : order_pos_of (self, self->context_index);

  if (pos + 1 < (gint) self->order->len) {
    play_context_at (self, (gint) g_array_index (self->order, guint, pos + 1), handover);
    return TRUE;
  }

  /* End of the order. Repeat-all starts again -- reshuffled, so a repeated
   * context is not the same sequence twice. */
  if (self->repeat == SPOTIFYGTK_REPEAT_ALL) {
    rebuild_order (self, -1);
    if (self->order && self->order->len > 0) {
      play_context_at (self, (gint) g_array_index (self->order, guint, 0), handover);
      return TRUE;
    }
  }

  return FALSE;
}

static gboolean
advance_prev (SpotifyGtkNativeWindow *self)
{
  if (!self->play_context || !self->order)
    return FALSE;

  gint pos = self->order_pos >= 0 ? self->order_pos
                                  : order_pos_of (self, self->context_index);
  if (pos > 0) {
    /* Previous is a button press, not a handover: the current track stops at
     * once, so its title should go with it. */
    play_context_at (self, (gint) g_array_index (self->order, guint, pos - 1), FALSE);
    return TRUE;
  }
  if (self->repeat == SPOTIFYGTK_REPEAT_ALL && self->order->len > 0) {
    play_context_at (self, (gint) g_array_index (self->order, guint,
                                                 self->order->len - 1), FALSE);
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
  rebuild_order (self, -1);
  self->context_index = -1;

  for (guint i = 0; i < self->play_context->len; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (self->play_context, i);
    if (g_strcmp0 (t->uri, track->uri) == 0) {
      self->context_index = (gint) i;
      break;
    }
  }

  if (self->context_index >= 0)
    play_context_at (self, self->context_index, FALSE);
  else
    play_native_track (self, track, FALSE);  /* not in the snapshot; play it alone */
}




/*
 * Collection changes made anywhere else.
 *
 * Liking on a phone, the web player, or the official client leaves this one
 * showing whatever it read at sign-in -- the page keeps its list and every
 * heart keeps its state, and only a restart corrects them. Spotify publishes a
 * change event to the collection URI, so subscribing is the difference between
 * a client that reflects the account and one that reflects a snapshot of it.
 *
 * The event says something changed, not what, so the response is to re-read
 * the set and mark the page stale rather than to trust its payload.
 */
/*
 * Coalesce collection change events.
 *
 * Spotify publishes several per change -- the collection URI, its /json twin,
 * and a per-artist list -- and our own writes produce them too. Reacting to
 * each one meant re-reading the whole collection ten pages at a time, per
 * event. On a 4806-track library that is most of a minute of round trips to
 * learn something already known.
 */
#define COLLECTION_CHANGE_SETTLE_MS 2000

/* How long after our own write an incoming change is assumed to be its echo. */
#define LOCAL_WRITE_GRACE_US (8 * G_TIME_SPAN_SECOND)

static gboolean
on_collection_settled (gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  self->collection_change_id = 0;

  /*
   * Re-read the set, because a change elsewhere is the only way to learn about
   * it -- but do NOT refetch the Liked Songs page. That is thousands of tracks
   * and their art to reflect a single addition, and the page is marked stale
   * so the next visit pays for it only if there is someone to see it.
   */
  /*
   * Skip the re-read when this change was ours. Our own write already updated
   * the set and the rows, so re-reading the whole collection to learn it costs
   * ten round trips to arrive back where we started -- and every like fires
   * this, because the subscription cannot tell whose change it is.
   *
   * An external change inside the same window is missed and picked up by the
   * next event or the next sign-in, which is a fair trade against making every
   * like re-read the library.
   */
  SPOTIFYGTK_DEBUG ("collection: change event settled");
  gint64 since = g_get_monotonic_time () - self->last_local_write_us;
  if (self->last_local_write_us != 0 && since < LOCAL_WRITE_GRACE_US) {
    /* Our own echo. The set and the page were updated when the action was
     * taken, so there is nothing here to learn and nothing to invalidate. */
    return G_SOURCE_REMOVE;
  }

  /*
   * A change from somewhere else -- another device, or the web player. Only
   * here is a refetch worth its cost: the URI arrived without any metadata, so
   * a row for it cannot be built locally the way a local like can.
   */
  spotifygtk_native_window_reload_liked (self);
  if (self->liked_page)
    spotifygtk_liked_songs_page_invalidate (self->liked_page);
  return G_SOURCE_REMOVE;
}

static void
on_collection_changed (MercuryResponse *response, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  (void) response;

  if (self->collection_change_id)
    g_source_remove (self->collection_change_id);
  self->collection_change_id =
    g_timeout_add (COLLECTION_CHANGE_SETTLE_MS, on_collection_settled, self);
}

static void
subscribe_collection_changes (SpotifyGtkNativeWindow *self)
{
  if (self->collection_sub)
    return;

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  g_autofree gchar *user = m ? spotifygtk_native_session_dup_username (self->session)
                             : NULL;
  if (!m || !user)
    return;

  g_autofree gchar *uri =
    g_strdup_printf ("hm://collection/collection/%s/json", user);
  self->collection_sub =
    spotifygtk_mercury_subscribe (m, uri, on_collection_changed, self);
}

static void set_row_liked_for_uri (SpotifyGtkNativeWindow *self,
                                  const gchar *uri, gboolean liked);


typedef struct {
  gchar   *uri;
  gboolean liked;
} PendingLike;

static void
pending_like_free (gpointer data)
{
  PendingLike *p = data;
  g_free (p->uri);
  g_free (p);
}

/*
 * Replay likes made while the connection was down. Called when the session
 * reaches READY, which is the first moment a write can succeed.
 *
 * Runs before the reconnect's own reload of the liked set, so the writes are
 * on the wire before the read that races them. That read can still land first
 * and show pre-write state, which is why this deliberately does not stamp
 * last_local_write_us: the grace window exists to suppress re-reads caused by
 * our own writes during normal use, but here a re-read is exactly what
 * resolves the race. The change event these writes provoke is left free to
 * trigger one, and the set settles a couple of seconds later.
 */
static void
flush_pending_likes (SpotifyGtkNativeWindow *self)
{
  if (!self->pending_likes || self->pending_likes->len == 0)
    return;

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  g_autofree gchar *user = m ? spotifygtk_native_session_dup_username (self->session)
                             : NULL;
  if (!m || !user)
    return;

  g_autoptr(GPtrArray) queued = g_steal_pointer (&self->pending_likes);
  g_message ("replaying %u liked-songs change(s) held while offline",
             queued->len);

  for (guint i = 0; i < queued->len; i++) {
    PendingLike *p = g_ptr_array_index (queued, i);
    const gchar *uris[] = { p->uri };
    spotifygtk_collection_v2_write (m, user, SPOTIFYGTK_COLLECTION_SET_LIKED,
                                    uris, 1, !p->liked, NULL, NULL);
  }
}

/* ── Liked Songs state ──────────────────────────────────────────────────────
 *
 * Every list shows a heart and every context menu offers "Add" or "Remove"
 * based on this. Without it, nothing is known to be liked until the user likes
 * it in this session, so the menu offered "Add" on tracks already saved and a
 * restart was the only way to see the truth.
 *
 * Read once at sign-in through collection v2, which pages -- so the whole set
 * arrives however large it is, rather than being capped by a request size.
 */
#define LIKED_PAGE_SIZE 500

static void liked_fetch_page (SpotifyGtkNativeWindow *self, const gchar *token);

typedef struct {
  SpotifyGtkNativeWindow *window;
  gchar                  *next;
} LikedPageResult;

static gboolean
liked_page_to_ui (gpointer data)
{
  LikedPageResult *r = data;
  SpotifyGtkNativeWindow *self = r->window;

  /* The set was filled as the page arrived; only what is on screen needs
   * repainting. Fanning every URI out to every list is what made a large
   * library take tens of seconds to sign in. */
  for (guint i = 0; self->track_lists && i < self->track_lists->len; i++)
    spotifygtk_track_list_refresh_liked (g_ptr_array_index (self->track_lists, i));

  if (r->next && *r->next)
    liked_fetch_page (self, r->next);
  else {
    /*
     * Swap, do not merge. A track unliked elsewhere has to disappear from the
     * set, and only a wholesale replacement expresses that -- but the old set
     * stays live until this instant, so nothing ever observes a partial one.
     */
    if (self->liked_building) {
      GHashTable *old = self->liked_uris;
      self->liked_uris = g_steal_pointer (&self->liked_building);
      for (guint li = 0; self->track_lists && li < self->track_lists->len; li++)
        spotifygtk_track_list_set_liked_set (
          g_ptr_array_index (self->track_lists, li), self->liked_uris);
      g_hash_table_unref (old);
    }
    g_message ("liked: %u track(s) known", g_hash_table_size (self->liked_uris));
    /*
     * Armed only now. Before the read completes an empty set means "not yet
     * known", and filtering against it would blank the page; once the last
     * page has arrived an empty set genuinely means nothing is liked, which is
     * a state the filter has to be able to express -- removing the last track
     * is exactly when a stale refetch would otherwise put it back.
     */
    spotifygtk_liked_songs_page_set_liked_filter (self->liked_page, self->liked_uris);

    /* The bar may be showing a track whose state just arrived, or changed on
     * another device. */
    if (self->current_track_uri)
      spotifygtk_playback_bar_set_liked (self->playback_bar,
        g_hash_table_contains (self->liked_uris, self->current_track_uri));
  }

  g_free (r->next);
  g_free (r);
  return G_SOURCE_REMOVE;
}

static void
on_liked_page (gboolean ok, guint16 status, SpotifyCollectionItem *items,
               guint n_items, const gchar *next_token, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  if (!ok) {
    g_warning ("liked: page read failed (status %u)", status);
    return;
  }

  LikedPageResult *r = g_new0 (LikedPageResult, 1);
  r->window = self;
  r->next   = g_strdup (next_token);

  for (guint i = 0; i < n_items; i++) {
    if (!items[i].uri || items[i].is_removed)
      continue;
    g_hash_table_add (self->liked_building ? self->liked_building : self->liked_uris,
                      g_strdup (items[i].uri));
  }

  /* The read runs off the main loop; widgets are only touched from it. */
  g_idle_add (liked_page_to_ui, r);
}

static void
liked_fetch_page (SpotifyGtkNativeWindow *self, const gchar *token)
{
  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  if (!m) {
    g_message ("liked: no session yet; state will load once signed in");
    return;
  }
  g_autofree gchar *user = spotifygtk_native_session_dup_username (self->session);
  if (!user)
    return;

  spotifygtk_collection_v2_read_page (m, user, SPOTIFYGTK_COLLECTION_SET_LIKED,
                                      token, LIKED_PAGE_SIZE,
                                      on_liked_page, self);
}

static void
spotifygtk_native_window_reload_liked (SpotifyGtkNativeWindow *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_NATIVE_WINDOW (self));

  g_clear_pointer (&self->liked_building, g_hash_table_unref);
  self->liked_building = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, NULL);
  liked_fetch_page (self, NULL);
}

/*
 * Apply a liked mark to every list that might be showing the track. The window
 * keeps several -- album, playlist, search, liked -- and a track can appear in
 * more than one at a time, so this fans out rather than guessing which is
 * frontmost.
 */
static void
set_row_liked_for_uri (SpotifyGtkNativeWindow *self, const gchar *uri, gboolean liked)
{
  /* The shared set is the state; the lists only need their visible rows
   * repainted. Nothing here walks the collection. */
  if (liked)
    g_hash_table_add (self->liked_uris, g_strdup (uri));
  else
    g_hash_table_remove (self->liked_uris, uri);

  for (guint i = 0; self->track_lists && i < self->track_lists->len; i++)
    spotifygtk_track_list_refresh_liked (g_ptr_array_index (self->track_lists, i));
}

/*
 * Liked Songs, from the row context menu.
 *
 * The write goes over collection v2, which is additive -- it names one track
 * and leaves the rest alone. The row indicator is updated straight away rather
 * than waiting for the round trip: the write is a delta the server accepts or
 * refuses outright, and a heart that lags a click by 200ms reads as broken.
 * A refusal puts it back.
 */
typedef struct {
  SpotifyGtkNativeWindow *window;
  gchar                  *uri;
  gboolean                liked;
} LikeUiCtx;

static gboolean
like_result_to_ui (gpointer data)
{
  LikeUiCtx *ctx = data;
  /*
   * Only reached when the write failed. The heart and the set are put back by
   * hand; the page is refetched rather than reconstructed, because a row that
   * was removed locally cannot be restored to its old position from here --
   * only the server knows the order.
   */
  set_row_liked_for_uri (ctx->window, ctx->uri, !ctx->liked);
  if (ctx->liked)
    g_hash_table_remove (ctx->window->liked_uris, ctx->uri);
  else
    g_hash_table_add (ctx->window->liked_uris, g_strdup (ctx->uri));

  if (ctx->window->liked_page) {
    spotifygtk_liked_songs_page_invalidate (ctx->window->liked_page);
    spotifygtk_liked_songs_page_refresh (ctx->window->liked_page);
  }
  g_free (ctx->uri);
  g_free (ctx);
  return G_SOURCE_REMOVE;
}

static void
on_like_write_done (gboolean ok, guint16 status, gpointer user_data)
{
  LikeUiCtx *ctx = user_data;
  SPOTIFYGTK_DEBUG ("like: write returned ok=%d status=%u for %s",
                    ok, status, ctx->uri);
  if (ok) {
    /*
     * Nothing to do. The row, the heart and the liked set were all updated
     * when the action was taken, and this only confirms it.
     *
     * This used to invalidate the page, which looked harmless and was not: an
     * invalidate drops the session's cached listing, so the next visit to
     * Liked Songs refetched several thousand tracks and their art to learn
     * something already known. One like made the page slow to open again.
     */
    g_free (ctx->uri);
    g_free (ctx);
    return;
  }
  g_warning ("collection write refused (status %u); reverting", status);
  g_idle_add (like_result_to_ui, ctx);   /* back to the main loop to touch widgets */
}

static void
list_set_liked (SpotifyGtkNativeWindow *self, gpointer track_ptr, gboolean liked)
{
  const SpotifyNativeTrack *track = track_ptr;
  if (!track || !track->uri)
    return;

  set_row_liked_for_uri (self, track->uri, liked);
  if (liked)
    g_hash_table_add (self->liked_uris, g_strdup (track->uri));
  else
    g_hash_table_remove (self->liked_uris, track->uri);

  /*
   * Drive the page from the action rather than from the write callback.
   *
   * Hanging the refresh off the response meant nothing moved until a round
   * trip completed, and nothing at all if that callback never arrived. The
   * user has just said what they want; the list can say it back immediately.
   *
   * The row is added or removed outright instead of refetching, because a
   * refetch fired now races the server: the write and the read are different
   * services, and the read can still return the old set and undo it. We have
   * the whole track here, not just its URI, so the row can be built without
   * asking anyone.
   *
   * The page is deliberately not invalidated afterwards. It is already correct,
   * and invalidating would throw away the session's cached listing and make the
   * next visit refetch the whole library. Reconciliation happens on the next
   * genuine change from elsewhere, or the next sign-in.
   */
  if (self->liked_page) {
    if (liked)
      spotifygtk_liked_songs_page_add_track (self->liked_page, track);
    else
      spotifygtk_liked_songs_page_remove_track (self->liked_page, track->uri);
  }

  LikeUiCtx *ctx = g_new0 (LikeUiCtx, 1);
  ctx->window = self;
  ctx->uri    = g_strdup (track->uri);
  ctx->liked  = liked;

  /*
   * Written on the session's Mercury rather than the engine's. The engine only
   * has one while a track is running, which would have made liking depend on
   * having played something first; the session's exists from sign-in.
   */
  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  g_autofree gchar *user = m ? spotifygtk_native_session_dup_username (self->session)
                             : NULL;
  if (!m || !user) {
    /*
     * Almost always a dropped AP connection rather than a genuine signed-out
     * state; get_mercury() kicks a reconnect when it sees one.
     *
     * Held rather than discarded. The optimistic update has already happened,
     * so throwing the click away leaves the UI showing a change the server was
     * never told about -- and the user has no way to know. It is replayed once
     * the session is back.
     */
    PendingLike *p = g_new0 (PendingLike, 1);
    p->uri   = g_strdup (track->uri);
    p->liked = liked;
    if (!self->pending_likes)
      self->pending_likes = g_ptr_array_new_with_free_func (pending_like_free);
    g_ptr_array_add (self->pending_likes, p);

    g_warning ("no live connection; holding %s of %s until the session is back",
               liked ? "add" : "remove", track->uri);
    SPOTIFYGTK_DEBUG ("like: queued (%u pending)", self->pending_likes->len);
    g_free (ctx->uri);
    g_free (ctx);
    return;
  }

  SPOTIFYGTK_DEBUG ("like: %s %s (set now %u)", liked ? "add" : "remove",
                    track->uri, g_hash_table_size (self->liked_uris));
  self->last_local_write_us = g_get_monotonic_time ();

  const gchar *uris[] = { track->uri };
  spotifygtk_collection_v2_write (m, user, SPOTIFYGTK_COLLECTION_SET_LIKED,
                                  uris, 1, !liked, on_like_write_done, ctx);
}


/*
 * The playback bar's heart, for whatever is playing.
 *
 * Shares list_set_liked()'s path so the bar, the rows and the context menus
 * cannot disagree: one write, one set, one fan-out.
 */
/* Shuffle keeps what is playing and reorders what follows, so toggling it
 * mid-track is not a jump cut. */
static void
on_shuffle_toggled (SpotifyGtkPlaybackBar *bar, gboolean enabled, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  (void) bar;

  self->shuffle = enabled;
  spotifygtk_settings_set_shuffle (spotifygtk_settings_get_default (), enabled);
  rebuild_order (self, self->context_index);
  refresh_transport_and_queue (self);
}

static void
on_repeat_changed (SpotifyGtkPlaybackBar *bar, guint mode, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  (void) bar;

  self->repeat = (SpotifyGtkRepeatMode) mode;
  spotifygtk_settings_set_repeat (spotifygtk_settings_get_default (), mode);
  refresh_transport_and_queue (self);   /* repeat-all makes Next reachable at the end */
}

static void
on_playback_like_toggled (SpotifyGtkPlaybackBar *bar, gboolean liked, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  (void) bar;

  if (!self->current_track_uri) {
    spotifygtk_playback_bar_set_liked (self->playback_bar, FALSE);
    return;
  }

  /*
   * The real track, not a URI in a bare struct.
   *
   * This used to hand list_set_liked a SpotifyNativeTrack with nothing but a
   * uri set, which is enough to write the collection but not to draw a row --
   * so liking from the bar inserted "Unknown track" into Liked Songs while the
   * context menu, which passes the row's own track, inserted it properly.
   *
   * display_tracks holds what was started, keyed by uri, for exactly this kind
   * of after-the-fact lookup.
   */
  const SpotifyNativeTrack *known =
    self->display_tracks ? g_hash_table_lookup (self->display_tracks,
                                                self->current_track_uri)
                         : NULL;
  if (known) {
    list_set_liked (self, (gpointer) known, liked);
    return;
  }

  SpotifyNativeTrack t = { 0 };
  t.uri = self->current_track_uri;
  list_set_liked (self, &t, liked);
}

static void
on_list_add_to_liked (SpotifyGtkTrackList *list, gpointer track_ptr, gpointer user_data)
{
  (void) list;
  list_set_liked (user_data, track_ptr, TRUE);
}

static void
on_list_remove_from_liked (SpotifyGtkTrackList *list, gpointer track_ptr, gpointer user_data)
{
  (void) list;
  list_set_liked (user_data, track_ptr, FALSE);
}



/* ── Playlists page ─────────────────────────────────────────────────────────
 *
 * A card is added once, complete. It was previously added immediately and then
 * patched twice -- once with its name, once with its cover -- by splicing the
 * model. That is what "the images refresh and then it crashes" was: each
 * splice destroys the item a card is bound to, so the view unbinds and rebinds
 * mid-flight while a cover load is still holding a borrowed pointer to that
 * card's image. Clicking during the churn walked a widget tree that was being
 * rebuilt underneath it.
 *
 * So the name and the cover are resolved first and the card is built from the
 * finished pieces. Cards appear one at a time as their lookups land, which
 * looks near enough the same and cannot tear anything down.
 */
typedef struct {
  SpotifyGtkNativeWindow *window;
  gchar                  *uri;
  gchar                  *name;      /* from the playlist head */
  guint                   generation;
} PlaylistCard;

static void
playlist_card_free (gpointer data)
{
  PlaylistCard *c = data;
  g_free (c->uri);
  g_free (c->name);
  g_free (c);
}

static void
on_playlist_card_activated (SpotifyGtkAlbumGrid *grid, const gchar *uri,
                            const gchar *name, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  (void) grid;
  if (uri)
    navigate_to_context (self, uri, name && *name ? name : "Playlist", "Playlist");
}

/* Second step: the first track, for a cover. Adds the finished card. */
static void
on_playlist_cover_tracks (GObject *source, GAsyncResult *result, gpointer user_data)
{
  PlaylistCard *c = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(GPtrArray) tracks = spotifygtk_native_session_load_tracks_finish (
    SPOTIFYGTK_NATIVE_SESSION (source), result, &err);

  /* A reload started while this was in flight owns the grid now; anything from
   * the previous pass would append duplicates. */
  if (!c->window->playlists_grid || c->generation != c->window->playlists_generation) {
    playlist_card_free (c);
    return;
  }

  const gchar *cover = NULL;
  if (tracks && tracks->len > 0) {
    const SpotifyNativeTrack *t = g_ptr_array_index (tracks, 0);
    if (t) cover = t->cover_id;
  }

  spotifygtk_album_grid_resolve_card (c->window->playlists_grid, c->uri,
                                      c->name ? c->name : c->uri, "Playlist", cover);
  playlist_card_free (c);
}

/* First step: the playlist head, for a name. */
static void
on_playlist_card_name (MercuryResponse *response, gpointer user_data)
{
  PlaylistCard *c = user_data;

  if (!c->window->playlists_grid || c->generation != c->window->playlists_generation) {
    playlist_card_free (c);
    return;
  }

  if (response && response->parts && response->parts->len > 0) {
    gsize len = 0;
    const guint8 *d = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &len);
    const guint8 *attrs = NULL; gsize alen = 0;
    const guint8 *name = NULL; gsize nlen = 0;
    if (pb_find_bytes_field (d, len, 3, &attrs, &alen) &&
        pb_find_bytes_field (attrs, alen, 1, &name, &nlen))
      c->name = g_strndup ((const gchar *) name, nlen);
  }

  /* One track is enough for a cover, and asking for one keeps this cheap on a
   * library of many playlists. */
  spotifygtk_native_session_load_tracks (c->window->session, c->uri, 1, NULL,
                                         on_playlist_cover_tracks, c);
}

/*
 * Set the playlists status line, hiding it when there is nothing to say.
 *
 * Blanking the text is not enough: an empty label still claims a line's
 * height, and this one sits below the grid, so the grid gave up a strip of
 * itself to a label showing nothing. The albums page already does this --
 * see library_page.c -- which is why it never showed the same gap.
 */
static void
set_playlists_status (SpotifyGtkNativeWindow *self, const gchar *message)
{
  if (!self->playlists_status)
    return;
  gboolean have = message && *message;
  gtk_label_set_text (GTK_LABEL (self->playlists_status), have ? message : "");
  gtk_widget_set_visible (self->playlists_status, have);
}

static void
on_page_playlists_listed (gboolean ok, gint32 status, SpotifyPlaylistEntry *entries,
                          guint n_entries, gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  self->playlists_loading = FALSE;
  if (!self->playlists_grid)
    return;

  /* Invalidates anything still in flight from a previous load. */
  self->playlists_generation++;
  spotifygtk_album_grid_clear (self->playlists_grid);

  if (!ok) {
    set_playlists_status (self, "Couldn\u2019t load your playlists.");
    g_warning ("playlists: rootlist read failed (status %d)", status);
    return;
  }
  if (n_entries == 0) {
    set_playlists_status (self, "No playlists yet.");
    return;
  }
  set_playlists_status (self, NULL);
  self->playlists_loaded = TRUE;

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  if (!m)
    return;

  /*
   * Cards go up straight away, knowing only their URIs; each fetches its own
   * name and cover when it is scrolled into view.
   *
   * Resolving them all here cost two round trips per playlist -- a head read
   * for the name, then a one-track context resolve for the cover -- issued for
   * all 59 at once and serialised over the AP connection. That is what made
   * this page take fifteen seconds to open, most of it for cards below the
   * fold.
   */
  guint added = 0;
  for (guint i = 0; i < n_entries; i++) {
    if (!entries[i].uri)
      continue;

    /* The rootlist interleaves folder markers with playlists. They are not
     * playlists, have no head to read, and resolving them produced nothing but
     * "could not find an upstream service" warnings. */
    if (!g_str_has_prefix (entries[i].uri, "spotify:playlist:"))
      continue;

    spotifygtk_album_grid_add_pending_card (self->playlists_grid, entries[i].uri,
                                            entries[i].name ? entries[i].name
                                                            : "Playlist",
                                            "Playlist");
    added++;
  }

  if (added == 0)
    set_playlists_status (self, "No playlists yet.");
  (void) m;
}

/* One card has come into view and wants its name and cover. */
static void
on_playlist_card_needs_resolve (SpotifyGtkAlbumGrid *grid, const gchar *uri,
                                gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  (void) grid;

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  if (!m || !uri)
    return;

  self->playlists_resolved++;
  SPOTIFYGTK_DEBUG ("playlists: resolving card %u (%s)", self->playlists_resolved, uri);

  PlaylistCard *c = g_new0 (PlaylistCard, 1);
  c->window     = self;
  c->uri        = g_strdup (uri);
  c->generation = self->playlists_generation;

  const gchar *id = strrchr (uri, ':');
  g_autofree gchar *head =
    g_strdup_printf ("hm://playlist/v2/playlist/%s", id ? id + 1 : uri);
  spotifygtk_mercury_request_full (m, MERCURY_METHOD_GET, "GET", head, NULL,
                                   on_playlist_card_name, c);
}

static void
reload_playlists (SpotifyGtkNativeWindow *self)
{
  /*
   * playlists_loaded is only set once the rootlist has come back, so it does
   * not stop a second call made while the first is still out -- and startup
   * makes exactly that pair, navigating to the visible page twice. Each one
   * cleared the grid and re-added every card, so every card resolved twice.
   */
  if (self->playlists_loading)
    return;

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  g_autofree gchar *user = m ? spotifygtk_native_session_dup_username (self->session)
                             : NULL;
  if (!m || !user)
    return;

  self->playlists_loading = TRUE;
  spotifygtk_playlist_list (m, user, on_page_playlists_listed, self);
}

/* ── Add to Playlist ────────────────────────────────────────────────────────
 *
 * The rootlist gives URIs but no names, so a chooser that showed only URIs
 * would be useless. Names come from reading each playlist's head, which is one
 * request per entry -- fine for a personal library, and the rows fill in as
 * they arrive rather than blocking the dialog on all of them.
 */
typedef struct {
  SpotifyGtkNativeWindow *window;
  gchar                  *track_uri;
  GtkWindow              *dialog;
  GtkWidget              *list_box;
} PlaylistPick;

static void
playlist_pick_free (gpointer data)
{
  PlaylistPick *p = data;
  g_free (p->track_uri);
  g_free (p);
}

static void
on_playlist_add_done (gboolean ok, gint32 status, gpointer user_data)
{
  gchar *name = user_data;
  if (ok)
    g_message ("playlist: added to %s", name);
  else
    g_warning ("playlist: add to %s failed (status %d)", name, status);
  g_free (name);
}

static void
on_pick_row_activated (GtkButton *button, gpointer user_data)
{
  PlaylistPick *p = user_data;
  const gchar *uri = g_object_get_data (G_OBJECT (button), "playlist-uri");

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (p->window->session);
  if (m && uri) {
    const gchar *tracks[] = { p->track_uri };
    spotifygtk_playlist_add_tracks (m, uri, tracks, 1, on_playlist_add_done,
                                    g_strdup (gtk_button_get_label (button)));
  }
  gtk_window_destroy (p->dialog);
}

static void
on_playlist_name_read (MercuryResponse *response, gpointer user_data)
{
  GtkWidget *button = user_data;

  if (!response || !response->parts || response->parts->len == 0) {
    g_object_unref (button);
    return;
  }
  gsize len = 0;
  const guint8 *d = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &len);

  /* SelectedListContent.attributes(3) -> ListAttributes.name(1) */
  const guint8 *attrs = NULL; gsize alen = 0;
  const guint8 *name = NULL; gsize nlen = 0;
  if (pb_find_bytes_field (d, len, 3, &attrs, &alen) &&
      pb_find_bytes_field (attrs, alen, 1, &name, &nlen)) {
    g_autofree gchar *n = g_strndup ((const gchar *) name, nlen);
    gtk_button_set_label (GTK_BUTTON (button), n);
  }
  g_object_unref (button);
}

static void
on_new_playlist_response (gboolean ok, gint32 status, const gchar *uri,
                          gpointer user_data)
{
  PlaylistPick *p = user_data;
  if (!ok) {
    g_warning ("playlist: create failed (status %d)", status);
  } else {
    g_message ("playlist: created %s", uri);
    SpotifyMercury *m = spotifygtk_native_session_get_mercury (p->window->session);
    if (m) {
      const gchar *tracks[] = { p->track_uri };
      spotifygtk_playlist_add_tracks (m, uri, tracks, 1, on_playlist_add_done,
                                      g_strdup ("the new playlist"));
    }
  }
  playlist_pick_free (p);
}

static void
on_new_playlist_clicked (GtkButton *button, gpointer user_data)
{
  PlaylistPick *p = user_data;
  (void) button;

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (p->window->session);
  g_autofree gchar *user = m ? spotifygtk_native_session_dup_username (p->window->session)
                             : NULL;
  if (!m || !user) {
    gtk_window_destroy (p->dialog);
    return;
  }

  /* Carried over so the track can be added once the playlist exists. */
  PlaylistPick *owned = g_new0 (PlaylistPick, 1);
  owned->window    = p->window;
  owned->track_uri = g_strdup (p->track_uri);

  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree gchar *stamp = g_date_time_format (now, "%Y-%m-%d %H:%M");
  g_autofree gchar *name = g_strdup_printf ("New Playlist %s", stamp);

  spotifygtk_playlist_create (m, user, name, on_new_playlist_response, owned);
  gtk_window_destroy (p->dialog);
}

static void
on_playlists_listed (gboolean ok, gint32 status, SpotifyPlaylistEntry *entries,
                     guint n_entries, gpointer user_data)
{
  PlaylistPick *p = user_data;

  if (!ok)
    g_warning ("playlist: could not read the rootlist (status %d)", status);

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (p->window->session);

  for (guint i = 0; i < n_entries; i++) {
    if (!entries[i].uri)
      continue;
    /* Labelled with the URI until the name arrives, so the row is clickable
     * immediately rather than after every lookup has returned. */
    GtkWidget *row = gtk_button_new_with_label (entries[i].uri);
    gtk_button_set_has_frame (GTK_BUTTON (row), FALSE);
    gtk_widget_add_css_class (row, "flat");
    if (GTK_IS_LABEL (gtk_button_get_child (GTK_BUTTON (row))))
      gtk_label_set_xalign (GTK_LABEL (gtk_button_get_child (GTK_BUTTON (row))), 0.0);
    g_object_set_data_full (G_OBJECT (row), "playlist-uri",
                            g_strdup (entries[i].uri), g_free);
    g_signal_connect (row, "clicked", G_CALLBACK (on_pick_row_activated), p);
    gtk_box_append (GTK_BOX (p->list_box), row);

    if (m) {
      const gchar *id = strrchr (entries[i].uri, ':');
      g_autofree gchar *head =
        g_strdup_printf ("hm://playlist/v2/playlist/%s", id ? id + 1 : entries[i].uri);
      spotifygtk_mercury_request_full (m, MERCURY_METHOD_GET, "GET", head, NULL,
                                       on_playlist_name_read, g_object_ref (row));
    }
  }
}

static void
on_list_add_to_playlist (SpotifyGtkTrackList *list, gpointer track_ptr, gpointer user_data)
{
  SpotifyGtkNativeWindow   *self  = user_data;
  const SpotifyNativeTrack *track = track_ptr;
  (void) list;
  if (!track || !track->uri)
    return;

  SpotifyMercury *m = spotifygtk_native_session_get_mercury (self->session);
  g_autofree gchar *user = m ? spotifygtk_native_session_dup_username (self->session)
                             : NULL;
  if (!m || !user) {
    g_warning ("cannot add to a playlist: not signed in yet");
    return;
  }

  GtkWidget *dialog = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (dialog), "Add to Playlist");
  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
  gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (self));
  gtk_window_set_default_size (GTK_WINDOW (dialog), 320, 400);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);

  PlaylistPick *p = g_new0 (PlaylistPick, 1);
  p->window    = self;
  p->track_uri = g_strdup (track->uri);
  p->dialog    = GTK_WINDOW (dialog);

  GtkWidget *new_btn = gtk_button_new_with_label ("New Playlist");
  g_signal_connect (new_btn, "clicked", G_CALLBACK (on_new_playlist_clicked), p);
  gtk_box_append (GTK_BOX (box), new_btn);
  gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  p->list_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), p->list_box);
  gtk_box_append (GTK_BOX (box), scroller);

  gtk_window_set_child (GTK_WINDOW (dialog), box);
  g_object_set_data_full (G_OBJECT (dialog), "pick", p, playlist_pick_free);

  spotifygtk_playlist_list (m, user, on_playlists_listed, p);
  gtk_window_present (GTK_WINDOW (dialog));
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
  if (!self->track_lists)
    self->track_lists = g_ptr_array_new ();
  if (!g_ptr_array_find (self->track_lists, list, NULL))
    g_ptr_array_add (self->track_lists, list);

  /* Borrowed, not copied: a list consults it on every bind, so a page built
   * before or after the read finishes is equally correct. */
  spotifygtk_track_list_set_liked_set (list, self->liked_uris);

  g_signal_connect (list, "track-activated", G_CALLBACK (on_list_track_activated), self);
  g_signal_connect (list, "add-to-queue",    G_CALLBACK (on_list_add_to_queue),    self);
  g_signal_connect (list, "go-to-album",     G_CALLBACK (on_list_go_to_album),     self);
  g_signal_connect (list, "go-to-artist",    G_CALLBACK (on_list_go_to_artist),    self);
  g_signal_connect (list, "add-to-liked",      G_CALLBACK (on_list_add_to_liked),      self);
  g_signal_connect (list, "remove-from-liked", G_CALLBACK (on_list_remove_from_liked), self);
  g_signal_connect (list, "add-to-playlist",   G_CALLBACK (on_list_add_to_playlist),   self);
}

/* Argument order adapter for the artist page's per-release lists. */
static void
wire_track_list_for (SpotifyGtkTrackList *list, gpointer user_data)
{
  wire_track_list (user_data, list);
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
  advance_next (self, FALSE);
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
    if (advance_next (self, TRUE))   /* the track ended on its own */
      return;
  }

  /* Nothing is going to arrive to release the latch now, and leaving it set
   * would freeze the progress bar for the rest of the session. */
  if (state == SPOTIFYGTK_PLAYER_IDLE || state == SPOTIFYGTK_PLAYER_ERROR)
    g_clear_pointer (&self->awaiting_uri, g_free);

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
  spotifygtk_artist_page_set_playing_uri (self->artist_page,
                                          self->current_track_uri, is_playing);
  spotifygtk_context_page_set_playing_uri (self->context_page, uri, is_playing);
  spotifygtk_artist_page_set_playing_uri (self->artist_page, uri, is_playing);

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

  /* The position still belongs to the outgoing track; painting it against the
   * new track's duration is what made the old song appear to start playing. */
  if (self->awaiting_uri)
    return;

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
    spotifygtk_artist_page_set_session (self->artist_page, session);
    spotifygtk_home_page_set_session (self->home_page, session);

    /* Liked state for every list and every context menu. Paged, so the size of
     * the collection does not matter. */
    flush_pending_likes (self);
    spotifygtk_native_window_reload_liked (self);
    subscribe_collection_changes (self);

    /*
     * Playlists are deliberately not loaded here.
     *
     * The rootlist carries no names or covers, so each entry costs a head read
     * of its own -- on a library of 59 playlists that is 59 round trips, and
     * they went out at sign-in whether or not the page was ever opened. They
     * share the AP connection with the collection load, so Liked Songs sat
     * behind all of them: measured, its metadata pages finished at 13:08:52
     * against a sign-in at 13:08:37, with the playlist reads filling the gap.
     *
     * navigate_raw() already loads them on first visit, guarded by
     * playlists_loaded, so this call only ever duplicated that work earlier and
     * at a worse moment.
     */

    /* Development aid: open straight onto a page so its load can be watched
     * without driving the UI by hand. */
    const gchar *start = g_getenv ("SPOTIFY_DEV_START_PAGE");
    if (start && *start)
      navigate_raw (self, start);
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
/*
 * Release or reload the artwork held by one page's lists and grids.
 *
 * Always used as a pair. Releasing without a reload is what made covers
 * disappear for good: a page returning to view does not rebind its rows, so
 * nothing else was ever going to ask for them again.
 */
static void
set_page_covers_loaded (SpotifyGtkNativeWindow *self, const gchar *page_name,
                        gboolean loaded)
{
  /*
   * Clear scroll deferral before asking for anything back.
   *
   * A deferrable request made while deferral is on is not queued, it is
   * dropped -- the callback fires with NULL and that is the end of it. The
   * flag is global and set by any list that is scrolling, including the one
   * being navigated away from, so the reload landed in exactly the window
   * where its requests would be discarded. Nothing then retried, because the
   * only retry is a scroll settle on that list, and a page nobody scrolls
   * never settles. That is the fifteen seconds of blank rows.
   *
   * A page being opened is the definition of art worth loading, so deferral
   * has no business applying to it.
   */
  if (loaded)
    spotifygtk_cover_set_deferred (FALSE);

  SpotifyGtkTrackList *list = NULL;

  if (g_strcmp0 (page_name, "liked") == 0)
    list = spotifygtk_liked_songs_page_get_list (self->liked_page);
  else if (g_strcmp0 (page_name, "search") == 0)
    list = spotifygtk_search_page_get_list (self->search_page);
  else if (g_strcmp0 (page_name, "context") == 0)
    list = spotifygtk_context_page_get_list (self->context_page);
  else if (g_strcmp0 (page_name, "artist") == 0)
    list = spotifygtk_artist_page_get_list (self->artist_page);

  if (list) {
    if (loaded) spotifygtk_track_list_reload_covers (list);
    else        spotifygtk_track_list_release_covers (list);
    return;
  }

  if (g_strcmp0 (page_name, "playlists") == 0 && self->playlists_grid) {
    if (loaded) spotifygtk_album_grid_reload_covers (self->playlists_grid);
    else        spotifygtk_album_grid_release_covers (self->playlists_grid);
  }
}

static void
navigate_raw (SpotifyGtkNativeWindow *self, const gchar *page_name)
{
  if (!gtk_stack_get_child_by_name (self->page_stack, page_name)) {
    g_warning ("No page named '%s'", page_name);
    return;
  }

  /*
   * Hand back the artwork of the page being left.
   *
   * A GtkStack keeps its non-visible children realised and bound, so their
   * rows and cards go on holding a reference to every cover they were showing
   * -- and the cover cache cannot free what a widget still references. Sitting
   * on one album page held the whole of Liked Songs' and Library's art behind
   * it, indefinitely, because nothing new was arriving to push the cache down.
   *
   * Released here rather than on a timer: leaving a page is the moment its art
   * stops being looked at, and the ids are kept so it returns when the page
   * does.
   */
  const gchar *leaving = gtk_stack_get_visible_child_name (self->page_stack);
  if (leaving && g_strcmp0 (leaving, page_name) != 0)
    set_page_covers_loaded (self, leaving, FALSE);

  gtk_stack_set_visible_child_name (self->page_stack, page_name);

  /* And ask the arriving page for its artwork back. */
  set_page_covers_loaded (self, page_name, TRUE);

  /* Load on first visit rather than at startup; the page no-ops the
   * refresh once it holds data. Home and Library are static for now. */
  if (g_strcmp0 (page_name, "liked") == 0)
    spotifygtk_liked_songs_page_refresh (self->liked_page);
  /*
   * Only if it has nothing yet. Reloading on every visit cleared the grid and
   * rebuilt it from two requests per playlist, so the page visibly emptied and
   * refilled each time it was opened -- the "playlists get unloaded" effect.
   * A playlist made elsewhere still appears, via the collection change
   * subscription and the next sign-in.
   */
  if (g_strcmp0 (page_name, "playlists") == 0 && !self->playlists_loaded)
    reload_playlists (self);

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
/* Render one track into the bar and the panel. Called either on the click, or
 * at the handover -- see the note in the activation path. */
static void
show_now_playing (SpotifyGtkNativeWindow *self, const SpotifyNativeTrack *track)
{
  if (!track)
    return;

  const gchar *name    = track->name    ? track->name    : "Unknown track";
  const gchar *artists = track->artists ? track->artists : "";
  const gchar *album   = track->album   ? track->album   : "";

  spotifygtk_playback_bar_set_track (self->playback_bar, name, artists);
  /* The bar's heart follows whatever is playing. */
  spotifygtk_playback_bar_set_liked (self->playback_bar,
    track->uri && self->liked_uris &&
    g_hash_table_contains (self->liked_uris, track->uri));
  spotifygtk_now_playing_panel_set_track (self->now_playing_panel, name, artists, album);

  spotifygtk_playback_bar_set_cover (self->playback_bar, track->cover_id);
  spotifygtk_now_playing_panel_set_cover (self->now_playing_panel, track->cover_id);
}

/* The sink reached a different track. Only now is it true to say so. */
static void
on_now_playing_changed (SpotifyNativePlayerService *player, const gchar *uri,
                        gpointer user_data)
{
  SpotifyGtkNativeWindow *self = user_data;
  (void) player;

  if (!uri || !self->display_tracks)
    return;

  /* Still catching up to a track the user picked: anything else the sink is
   * sounding on the way there is stale by definition. */
  if (self->awaiting_uri) {
    if (g_strcmp0 (uri, self->awaiting_uri) != 0)
      return;
    g_clear_pointer (&self->awaiting_uri, g_free);
  }

  const SpotifyNativeTrack *track = g_hash_table_lookup (self->display_tracks, uri);
  if (!track)
    return;

  g_free (self->current_track_uri);
  self->current_track_uri = g_strdup (uri);
  self->current_track_duration_ms = track->duration_ms;
  spotifygtk_native_window_set_progress (self, 0, track->duration_ms);
  show_now_playing (self, track);
}

static void
navigate_to_context (SpotifyGtkNativeWindow *self, const gchar *uri,
                     const gchar *title, const gchar *kind)
{
  /* Artists have their own page -- a hero, their popular tracks and their
   * releases as cards -- rather than the bare track list an album gets. */
  if (uri && g_str_has_prefix (uri, "spotify:artist:")) {
    spotifygtk_artist_page_show (self->artist_page, uri, title);
    navigate_raw (self, "artist");
    nav_record (self, "artist", uri, title, kind);
    return;
  }

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
  /* The artist page's banner. A larger radius than the art tiles because it
   * spans the page rather than sitting in a row of equals, and the same
   * palette entry so it reads as the same family of surface. */
  ".artist-hero { background-color: @art_bg; border-radius: 16px;"
  "  color: @art_glyph; }"
  /* The banner image is clipped to the panel it fills, or a covering image
   * would paint over the rounded corners and square them off. */
  ".artist-hero-art { border-radius: 16px; }"
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
  /* The row's liked mark is an indicator inside the duration column, not a
   * control -- no padding, no hover state, nothing to click. */
  ".row-like { padding: 0; }"

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
  /* Icons compiled into the binary -- the theme ships no outline heart. Added
   * before any widget asks for one; repeat calls are harmless. */
  gtk_icon_theme_add_resource_path (
    gtk_icon_theme_get_for_display (gdk_display_get_default ()),
    "/com/github/spotifygtk/SpotifyNative/icons");

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
  g_signal_connect (self->player, "now-playing-changed",
                    G_CALLBACK (on_now_playing_changed), self);
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
  self->artist_page  = spotifygtk_artist_page_new ();

  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->home_page), "home");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->search_page), "search");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->liked_page), "liked");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->library_page), "library");
  /*
   * Playlists has no data source yet, so this page exists to say so rather
   * than to hide the gap. It used to be a footer on the Library page, where it
   * permanently cost that grid a row of albums to display one sentence.
   *
   * It needs spclient's rootlist endpoint, which returns playlist4_external
   * protobuf rather than the JSON the catalog path parses. A playlist still
   * opens fine once its URI is known -- context-resolve handles
   * spotify:playlist:<id> like everything else.
   */
  {
    GtkWidget *pl = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (pl, 35);
    gtk_widget_set_margin_end (pl, 35);
    gtk_widget_set_margin_top (pl, 24);

    GtkWidget *pl_title = gtk_label_new ("Playlists");
    gtk_widget_add_css_class (pl_title, "title-text");
    gtk_label_set_xalign (GTK_LABEL (pl_title), 0.0);
    gtk_box_append (GTK_BOX (pl), pl_title);

    /*
     * Presented as cards, like albums. The rootlist gives only URIs, so a card
     * starts with its URI as the title and is completed as two further
     * requests land: the playlist's head for its name, and its first track for
     * a cover. Playlists carry no picture of their own -- the attributes hold
     * a name and nothing else -- so the cover is borrowed from the first track,
     * which is what an auto-generated playlist image is anyway.
     */
    self->playlists_grid = spotifygtk_album_grid_new_grid ();
    gtk_widget_set_vexpand (GTK_WIDGET (self->playlists_grid), TRUE);
    spotifygtk_album_grid_set_content_margins (self->playlists_grid, 0, 0);
    g_signal_connect (self->playlists_grid, "album-activated",
                      G_CALLBACK (on_playlist_card_activated), self);
    g_signal_connect (self->playlists_grid, "card-needs-resolve",
                      G_CALLBACK (on_playlist_card_needs_resolve), self);
    gtk_box_append (GTK_BOX (pl), GTK_WIDGET (self->playlists_grid));

    self->playlists_status = gtk_label_new ("Not signed in yet.");
    gtk_widget_add_css_class (self->playlists_status, "dim-text");
    gtk_label_set_xalign (GTK_LABEL (self->playlists_status), 0.0);
    gtk_box_append (GTK_BOX (pl), self->playlists_status);

    gtk_stack_add_named (self->page_stack, pl, "playlists");
  }

  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->settings_page), "settings");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->context_page), "context");
  gtk_stack_add_named (self->page_stack, GTK_WIDGET (self->artist_page), "artist");

  /* Every track list — search results, liked songs, and an opened album or
   * artist — routes activation, queueing and album/artist navigation through
   * the window via the same handlers. Home and Library have no lists yet. */
  wire_track_list (self, spotifygtk_search_page_get_list (self->search_page));
  wire_track_list (self, spotifygtk_liked_songs_page_get_list (self->liked_page));
  wire_track_list (self, spotifygtk_context_page_get_list (self->context_page));
  wire_track_list (self, spotifygtk_artist_page_get_list (self->artist_page));

  /* The artist page builds a list per release after this point, so it needs
   * the same wiring applied to each as it is created. */
  spotifygtk_artist_page_set_list_wire (self->artist_page, wire_track_list_for, self);

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
  g_signal_connect (self->playback_bar, "like-toggled",
                    G_CALLBACK (on_playback_like_toggled), self);
  /* Restore the persisted modes before anything can play, and without
   * emitting -- these setters are the source of the values, not a reaction. */
  {
    SpotifyGtkSettings *st = spotifygtk_settings_get_default ();
    self->shuffle = spotifygtk_settings_get_shuffle (st);
    self->repeat  = (SpotifyGtkRepeatMode) spotifygtk_settings_get_repeat (st);
    spotifygtk_playback_bar_set_modes (self->playback_bar, self->shuffle, self->repeat);
  }
  g_signal_connect (self->playback_bar, "shuffle-toggled",
                    G_CALLBACK (on_shuffle_toggled), self);
  g_signal_connect (self->playback_bar, "repeat-changed",
                    G_CALLBACK (on_repeat_changed), self);
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

  /* has_credentials, not has_valid_token: an hour-old access token refreshes
   * silently inside the session, and gating on validity put a login screen in
   * the way every time one expired. If the refresh genuinely fails the session
   * reports FAILED and the gate comes back up. */
  if (native_auth_has_credentials (self->auth)) {
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
  g_clear_pointer (&self->pending_likes, g_ptr_array_unref);
  g_clear_pointer (&self->display_tracks, g_hash_table_unref);
  g_clear_pointer (&self->awaiting_uri, g_free);
  g_clear_pointer (&self->liked_uris, g_hash_table_unref);
  g_clear_pointer (&self->liked_building, g_hash_table_unref);
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


/* Kept small enough to be worth reclaiming, large enough that restoring the
 * window does not repaint from an empty cache. */
#define COVER_SUSPENDED_BUDGET (4 * 1024 * 1024)

/*
 * GdkToplevel::suspended is true when the compositor says the surface is not
 * visible -- minimised, on another workspace, fully occluded. That is the one
 * moment cached art is provably not being looked at, so it is the one moment
 * worth dropping it: the cache otherwise holds its 48MB ceiling for the life
 * of the process.
 *
 * Deliberately not hooked to focus. Alt-tabbing away leaves the window on
 * screen, and throwing away art someone is still looking at to save a few
 * megabytes would trade a visible repaint for an invisible gain.
 */
static void
on_toplevel_suspended (GdkToplevel *toplevel, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec; (void) user_data;
  if (gdk_toplevel_get_state (toplevel) & GDK_TOPLEVEL_STATE_SUSPENDED)
    spotifygtk_cover_trim_to (COVER_SUSPENDED_BUDGET);
}

static void
on_window_realize (GtkWidget *widget, gpointer user_data)
{
  (void) user_data;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));
  if (GDK_IS_TOPLEVEL (surface))
    g_signal_connect (surface, "notify::state",
                      G_CALLBACK (on_toplevel_suspended), NULL);
}

static void
spotifygtk_native_window_init (SpotifyGtkNativeWindow *self)
{
  g_signal_connect (self, "realize", G_CALLBACK (on_window_realize), NULL);

  self->context_index = -1;
  self->liked_uris = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
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

/*
 * now_playing_panel.c — Right-side Now Playing panel implementation.
 */

#include "now_playing_panel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <libsoup/soup.h>
#include "settings.h"
#include "spotify/track_meta.h"

/* Decode target for the panel cover. The displayed size now follows the panel
 * width, so this is just "big enough for the widest the panel can get" (the
 * paned clamps it to 420) rather than the drawn size. */
#define ART_DECODE_PX 420
#include "cover_loader.h"
#include "smooth_scroll.h"

struct _SpotifyGtkNowPlayingPanel {
  GtkBox parent_instance;

  GtkImage   *album_art;    /* placeholder icon, shown when no cover */
  GtkPicture *album_pic;    /* the cover, scaled to fill */
  GtkLabel *track_label;
  GtkLabel *artist_label;
  GtkLabel *queue_heading;    /* "Next Up"; hidden when the queue is empty */
  GtkListBox *queue_list;
  GtkStack   *content_stack;
  GtkLabel   *lyrics_status;
  GtkButton  *lyrics_retry;
  GtkLabel   *lyrics_previous;
  GtkLabel   *lyrics_current;
  GtkLabel   *lyrics_next;
  GtkTextView *lyrics_plain;
  GtkWidget  *lyrics_timed_box;
  GtkWidget  *lyrics_timed_scroller;
  GtkWidget  *lyrics_plain_scroller;
  GtkCssProvider *lyrics_font_css;
  GdkDisplay *lyrics_font_display;
  GtkWidget  *art_section;   /* artwork + track info; what collapses */
  GtkButton  *collapse_btn;
  gboolean    collapsed;

  gboolean is_playing;

  gchar *lyrics_uri;
  gchar *lyrics_title;
  gchar *lyrics_artist;
  gchar *lyrics_album;
  gint64 lyrics_duration_ms;
  gint64 lyrics_position_ms;
  gint   lyrics_active_index;
  guint  lyrics_generation;
  guint  lyrics_center_tick_id;
  guint  lyrics_center_tick_frames;
  GPtrArray *lyrics_lines;
  GCancellable *lyrics_cancellable;
  SoupSession *lyrics_session;
  GHashTable *lyrics_cache;   /* track URI -> LyricsCacheEntry */
  GQueue lyrics_cache_order;  /* oldest URI first, bounded to 16 songs */
  gint64 lyrics_retry_until_us;
  gboolean online_lyrics_seen;
  guint lyrics_font_size_seen;
  gboolean lyrics_requested;       /* lazy: no lookup until Lyrics is opened */
};

typedef struct {
  GPtrArray *lines;
  gchar *plain;
  gint64 expires_us;
} LyricsCacheEntry;

typedef struct {
  GWeakRef panel;                   /* do not retain a closed window */
  guint generation;
  SoupMessage *message;              /* online status and Retry-After */
} LyricsRequest;

static void
lyrics_cache_entry_free (gpointer data)
{
  LyricsCacheEntry *entry = data;
  g_clear_pointer (&entry->lines, g_ptr_array_unref);
  g_free (entry->plain);
  g_free (entry);
}

static void
lyrics_request_free (LyricsRequest *request)
{
  g_clear_object (&request->message);
  g_weak_ref_clear (&request->panel);
  g_free (request);
}

static LyricsRequest *
lyrics_request_new (SpotifyGtkNowPlayingPanel *self)
{
  LyricsRequest *request = g_new0 (LyricsRequest, 1);
  g_weak_ref_init (&request->panel, self);
  request->generation = self->lyrics_generation;
  return request;
}

static gboolean
lyrics_request_current (LyricsRequest *request,
                        SpotifyGtkNowPlayingPanel *self)
{
  return self && request->generation == self->lyrics_generation;
}

G_DEFINE_FINAL_TYPE (SpotifyGtkNowPlayingPanel, spotifygtk_now_playing_panel, GTK_TYPE_BOX)

enum { COLLAPSE_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];
static void on_lyrics_setting_changed (SpotifyGtkSettings *settings,
                                       gpointer user_data);
static void lyrics_begin_load (SpotifyGtkNowPlayingPanel *self);
static void lyrics_update_line (SpotifyGtkNowPlayingPanel *self);
static gboolean lyrics_center_tick (GtkWidget *widget, GdkFrameClock *clock,
                                    gpointer user_data);

static void
lyrics_apply_font_size (SpotifyGtkNowPlayingPanel *self, guint pixels)
{
  if (!self->lyrics_font_css || self->lyrics_font_size_seen == pixels)
    return;
  self->lyrics_font_size_seen = pixels;
  guint adjacent = 13 + (pixels - 19) * 2 / 3;
  g_autofree gchar *css = g_strdup_printf (
    "label.lyrics-current { font-size: %upx; } "
    "label.lyrics-adjacent { font-size: %upx; } "
    "textview.lyrics-plain, textview.lyrics-plain text { font-size: %upx; }",
    pixels, adjacent, pixels);
  gtk_css_provider_load_from_string (self->lyrics_font_css, css);
}

static void
spotifygtk_now_playing_panel_dispose (GObject *object)
{
  SpotifyGtkNowPlayingPanel *self = SPOTIFYGTK_NOW_PLAYING_PANEL (object);
  if (self->lyrics_cancellable)
    g_cancellable_cancel (self->lyrics_cancellable);
  if (self->lyrics_center_tick_id) {
    gtk_widget_remove_tick_callback (self->lyrics_timed_scroller,
                                     self->lyrics_center_tick_id);
    self->lyrics_center_tick_id = 0;
  }
  g_clear_object (&self->lyrics_cancellable);
  g_clear_object (&self->lyrics_session);
  if (self->lyrics_font_css && self->lyrics_font_display)
    gtk_style_context_remove_provider_for_display (
      self->lyrics_font_display, GTK_STYLE_PROVIDER (self->lyrics_font_css));
  g_clear_object (&self->lyrics_font_css);
  g_clear_object (&self->lyrics_font_display);
  g_clear_pointer (&self->lyrics_lines, g_ptr_array_unref);
  g_clear_pointer (&self->lyrics_cache, g_hash_table_unref);
  while (!g_queue_is_empty (&self->lyrics_cache_order))
    g_free (g_queue_pop_head (&self->lyrics_cache_order));
  g_clear_pointer (&self->lyrics_uri, g_free);
  g_clear_pointer (&self->lyrics_title, g_free);
  g_clear_pointer (&self->lyrics_artist, g_free);
  g_clear_pointer (&self->lyrics_album, g_free);
  G_OBJECT_CLASS (spotifygtk_now_playing_panel_parent_class)->dispose (object);
}

static void
spotifygtk_now_playing_panel_class_init (SpotifyGtkNowPlayingPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_now_playing_panel_dispose;

  signals[COLLAPSE_REQUESTED] = g_signal_new ("collapse-requested",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

/* Collapsing hides the artwork and track info, leaving the header and the
 * queue. That is what the arrow in the mockup implies and what makes the
 * panel useful on a short window. */
static void
on_collapse_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkNowPlayingPanel *self = user_data;

  /* Collapsing the artwork alone left an empty panel; users read the button
   * as "close this pane". It now asks the window to hide the whole Now
   * Playing panel; the queue button in the playback bar reopens it. */
  g_signal_emit (self, signals[COLLAPSE_REQUESTED], 0);
  (void) button;
}

/* === Marquee title ===
 *
 * A long track title used to force the whole panel wider than the window,
 * pushing the artwork and queue off-screen, because a plain GtkLabel demands
 * its full text width as its minimum size. Wrapping the title in a clipping
 * scroller caps that: the scroller has a near-zero minimum width, so the panel
 * keeps its intended size and the title is clipped instead of overflowing.
 * When the title is wider than the panel it scrolls slowly back and forth --
 * the "radio ticker" look -- and when it fits it just sits still.
 */
typedef struct {
  GtkScrolledWindow *scroller;
  GtkWidget         *label;
  guint              tick_id;
  gint64             start_us;
} Marquee;

static gboolean marquee_tick (GtkWidget *widget, GdkFrameClock *clock,
                              gpointer user_data);

static void
marquee_start (Marquee *m)
{
  if (!m || m->tick_id || !m->label || !gtk_widget_get_mapped (m->label))
    return;
  m->start_us = 0;
  m->tick_id = gtk_widget_add_tick_callback (m->label, marquee_tick, m, NULL);
}

static void
on_marquee_geometry_or_text (GObject *object, GParamSpec *pspec,
                             gpointer user_data)
{
  Marquee *m = user_data;
  (void) object;
  (void) pspec;
  marquee_start (m);
}

static void
on_marquee_mapped (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  marquee_start (user_data);
}

static gboolean
marquee_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  Marquee *m = user_data;
  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (m->scroller);
  gdouble span = gtk_adjustment_get_upper (hadj) - gtk_adjustment_get_page_size (hadj);

  if (span <= 1.0) {                       /* title fits: hold at the start */
    gtk_adjustment_set_value (hadj, 0.0);
    m->start_us = 0;
    m->tick_id = 0;
    return G_SOURCE_REMOVE;
  }

  const gdouble speed = 32.0;              /* px per second */
  const gdouble pause = 1.6;               /* seconds held at each end */
  gdouble travel = span / speed;
  gdouble cycle  = 2.0 * (pause + travel);

  gint64 now = gdk_frame_clock_get_frame_time (clock);
  if (m->start_us == 0)
    m->start_us = now;
  gdouble t = fmod ((now - m->start_us) / (gdouble) G_USEC_PER_SEC, cycle);

  /* pause at left, scroll right, pause at right, scroll back -- a triangle. */
  gdouble v;
  if      (t < pause)                 v = 0.0;
  else if (t < pause + travel)        v = (t - pause) / travel * span;
  else if (t < 2.0 * pause + travel)  v = span;
  else                                v = span - (t - (2.0 * pause + travel)) / travel * span;

  gtk_adjustment_set_value (hadj, v);
  (void) widget;
  return G_SOURCE_CONTINUE;
}

/* A single-line label in a clipping, self-scrolling container. The label is
 * returned via *out_label so the caller can set its text; the marquee state is
 * stashed on it as "marquee" so a track change can restart the scroll. */
static GtkWidget *
build_marquee (GtkLabel **out_label, const gchar *css)
{
  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (scroller),
                                   GTK_ORIENTATION_VERTICAL);
  gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (scroller), FALSE);
  gtk_scrolled_window_set_min_content_width (GTK_SCROLLED_WINDOW (scroller), 0);
  gtk_widget_set_hexpand (scroller, TRUE);

  GtkWidget *label = gtk_label_new ("");
  gtk_widget_add_css_class (label, css);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), label);

  Marquee *m = g_new0 (Marquee, 1);
  m->scroller = GTK_SCROLLED_WINDOW (scroller);
  m->label = label;
  g_object_set_data (G_OBJECT (label), "marquee", m);
  g_signal_connect (label, "map", G_CALLBACK (on_marquee_mapped), m);
  g_signal_connect (label, "notify::label",
                    G_CALLBACK (on_marquee_geometry_or_text), m);
  g_signal_connect (scroller, "notify::width",
                    G_CALLBACK (on_marquee_geometry_or_text), m);
  g_object_set_data_full (G_OBJECT (label), "marquee-owner", m, g_free);

  *out_label = GTK_LABEL (label);
  return scroller;
}

static void
marquee_reset (GtkLabel *label)
{
  Marquee *m = g_object_get_data (G_OBJECT (label), "marquee");
  if (m) {
    m->start_us = 0;
    marquee_start (m);
  }
}

static void
on_lower_view_changed (GtkStack *stack, GParamSpec *pspec,
                       gpointer user_data)
{
  SpotifyGtkNowPlayingPanel *self = user_data;
  if (g_strcmp0 (gtk_stack_get_visible_child_name (stack), "lyrics") != 0)
    return;
  if (!self->lyrics_requested)
    lyrics_begin_load (self);
  else
    lyrics_update_line (self);
  (void) pspec;
}

static void
on_lyrics_retry_clicked (GtkButton *button, gpointer user_data)
{
  lyrics_begin_load (user_data);
  (void) button;
}

static GtkLabel *
new_lyric_label (const gchar *css)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_wrap (label, TRUE);
  gtk_label_set_wrap_mode (label, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_justify (label, GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (label, 0.5f);
  gtk_label_set_max_width_chars (label, 32);
  gtk_widget_set_hexpand (GTK_WIDGET (label), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (label), css);
  gtk_widget_add_css_class (GTK_WIDGET (label),
    g_strcmp0 (css, "section-heading") == 0
      ? "lyrics-current" : "lyrics-adjacent");
  return label;
}

static void
spotifygtk_now_playing_panel_init (SpotifyGtkNowPlayingPanel *self)
{
  self->lyrics_active_index = -2;
  self->lyrics_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, lyrics_cache_entry_free);
  g_queue_init (&self->lyrics_cache_order);
  self->online_lyrics_seen = spotifygtk_settings_get_online_lyrics (
    spotifygtk_settings_get_default ());
  g_signal_connect_object (spotifygtk_settings_get_default (), "changed",
                           G_CALLBACK (on_lyrics_setting_changed), self, 0);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "now-playing-panel");
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);

  /* Header: "Now Playing" + Collapse */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_start (header, 16);
  gtk_widget_set_margin_end (header, 16);
  gtk_widget_set_margin_top (header, 16);
  gtk_widget_set_margin_bottom (header, 8);

  GtkWidget *title = gtk_label_new ("Now Playing");
  gtk_widget_add_css_class (title, "normal-text");
  gtk_widget_set_hexpand (title, TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (header), title);

  /* This was a GtkLabel, which is why clicking it did nothing. It has to be
   * an actual button to be activatable at all. */
  self->collapse_btn = GTK_BUTTON (gtk_button_new_with_label ("Collapse ◂"));
  gtk_widget_add_css_class (GTK_WIDGET (self->collapse_btn), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (self->collapse_btn), "dim-text");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->collapse_btn), "Hide the artwork");
  g_signal_connect (self->collapse_btn, "clicked", G_CALLBACK (on_collapse_clicked), self);
  gtk_box_append (GTK_BOX (header), GTK_WIDGET (self->collapse_btn));

  gtk_box_append (GTK_BOX (self), header);

  /* Artwork and track info live in one box so the collapse button has a
   * single thing to hide. */
  self->art_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (self->art_section, GTK_ALIGN_START);

  /* A GtkAspectFrame rather than a fixed 220x220 request: the cover has to
   * stay square while tracking the panel's width, and a size request cannot
   * do both. ratio 1.0 with obey_child FALSE means "always square,
   * whatever the child would rather be", so a non-square cover cannot
   * stretch the box either. */
  /* Placeholder icon and the cover picture share one overlay: the icon draws
   * the dark box and the note glyph when there is no art, the picture fills
   * the frame when there is. A GtkImage cannot do the filling -- it renders a
   * paintable at a fixed pixel size, centred, which is why the cover
   * previously sat small inside a large square. GtkPicture scales its
   * paintable to the allocation, which is exactly what a cover wants. */
  self->album_art = GTK_IMAGE (gtk_image_new_from_icon_name ("audio-x-generic-symbolic"));
  gtk_image_set_pixel_size (self->album_art, 96);
  gtk_widget_add_css_class (GTK_WIDGET (self->album_art), "art-large");

  self->album_pic = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (self->album_pic, GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink (self->album_pic, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->album_pic), "art-large");
  gtk_widget_set_visible (GTK_WIDGET (self->album_pic), FALSE);

  GtkWidget *art_overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (art_overlay), GTK_WIDGET (self->album_art));
  gtk_overlay_add_overlay (GTK_OVERLAY (art_overlay), GTK_WIDGET (self->album_pic));

  /* The cover follows the panel width again, in a GtkAspectFrame so it stays
   * square while doing it (ratio 1.0, obey_child FALSE: always square, whatever
   * the child would rather be, so a non-square cover cannot stretch the box).
   *
   * This was previously pinned to a fixed square because covers appeared to
   * grow the whole right pane. That turned out to be the track title -- an
   * unconstrained GtkLabel demanding its full text width as the panel's
   * minimum -- not the artwork; the title is a clipping marquee now, so the
   * fixed square is no longer needed. The picture itself cannot force growth
   * either: can_shrink is TRUE, so its minimum width is zero and only its
   * natural size scales with the cover, which the paned clamp already bounds. */
  GtkWidget *art_frame = gtk_aspect_frame_new (0.5f, 0.5f, 1.0f, FALSE);
  gtk_aspect_frame_set_child (GTK_ASPECT_FRAME (art_frame), art_overlay);
  gtk_widget_set_hexpand (art_frame, TRUE);
  gtk_widget_set_valign (art_frame, GTK_ALIGN_START);
  gtk_widget_set_margin_start (art_frame, 20);
  gtk_widget_set_margin_end (art_frame, 20);
  gtk_widget_set_margin_top (art_frame, 8);
  gtk_box_append (GTK_BOX (self->art_section), art_frame);

  /* Track info */
  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start (info, 24);
  gtk_widget_set_margin_end (info, 24);
  gtk_widget_set_margin_top (info, 16);

  GtkWidget *track_marquee = build_marquee (&self->track_label, "normal-text");
  gtk_label_set_text (self->track_label, "Track Title");
  gtk_box_append (GTK_BOX (info), track_marquee);

  /* The artist/album line stays a plain ellipsizing label -- one moving line
   * reads as a ticker; two would just look restless. */
  self->artist_label = GTK_LABEL (gtk_label_new ("Artist • Album"));
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "dim-text");
  gtk_label_set_xalign (self->artist_label, 0.0);
  gtk_label_set_ellipsize (self->artist_label, PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->artist_label));

  gtk_box_append (GTK_BOX (self->art_section), info);
  gtk_widget_set_margin_bottom (self->art_section, 6);
  gtk_box_append (GTK_BOX (self), self->art_section);

  /* A shared lower slot avoids loading a second long widget tree alongside
   * the queue. Lyrics render in three reusable labels, not one row per line:
   * the latter would put another virtualisation/layout load on scrolling. */
  self->content_stack = GTK_STACK (gtk_stack_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->content_stack), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->content_stack), TRUE);

  GtkWidget *tabs = gtk_stack_switcher_new ();
  gtk_stack_switcher_set_stack (GTK_STACK_SWITCHER (tabs),
                               self->content_stack);
  gtk_widget_set_margin_start (tabs, 16);
  gtk_widget_set_margin_end (tabs, 16);
  gtk_widget_set_margin_top (tabs, 10);
  gtk_widget_set_margin_bottom (tabs, 8);
  gtk_box_append (GTK_BOX (self), tabs);

  GtkWidget *queue_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  /* Queue list, under a "Next Up" heading that hides itself when empty. */
  self->queue_heading = GTK_LABEL (gtk_label_new ("Next Up"));
  gtk_widget_add_css_class (GTK_WIDGET (self->queue_heading), "dim-text");
  gtk_label_set_xalign (self->queue_heading, 0.0);
  gtk_widget_set_margin_start (GTK_WIDGET (self->queue_heading), 20);
  gtk_widget_set_margin_end (GTK_WIDGET (self->queue_heading), 16);
  gtk_widget_set_margin_top (GTK_WIDGET (self->queue_heading), 30);
  gtk_widget_set_visible (GTK_WIDGET (self->queue_heading), FALSE);
  gtk_box_append (GTK_BOX (queue_box), GTK_WIDGET (self->queue_heading));

  GtkWidget *queue_scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (queue_scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (queue_scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (queue_scroller),
                                   GTK_ORIENTATION_VERTICAL);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (queue_scroller), FALSE);

  self->queue_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->queue_list, GTK_SELECTION_NONE);
  gtk_widget_set_margin_top (GTK_WIDGET (self->queue_list), 8);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (queue_scroller),
                                 GTK_WIDGET (self->queue_list));
  gtk_box_append (GTK_BOX (queue_box), queue_scroller);
  gtk_stack_add_titled (self->content_stack, queue_box, "queue", "Queue");

  GtkWidget *lyrics_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (lyrics_box, 20);
  gtk_widget_set_margin_end (lyrics_box, 20);
  gtk_widget_set_margin_top (lyrics_box, 20);
  self->lyrics_status = GTK_LABEL (gtk_label_new (
    "Play a song to see lyrics"));
  gtk_widget_add_css_class (GTK_WIDGET (self->lyrics_status), "dim-text");
  gtk_label_set_wrap (self->lyrics_status, TRUE);
  gtk_label_set_xalign (self->lyrics_status, 0.0f);
  gtk_box_append (GTK_BOX (lyrics_box), GTK_WIDGET (self->lyrics_status));
  self->lyrics_retry = GTK_BUTTON (gtk_button_new_with_label ("Retry lyrics"));
  gtk_widget_add_css_class (GTK_WIDGET (self->lyrics_retry), "flat");
  gtk_widget_set_visible (GTK_WIDGET (self->lyrics_retry), FALSE);
  g_signal_connect (self->lyrics_retry, "clicked",
                    G_CALLBACK (on_lyrics_retry_clicked), self);
  gtk_box_append (GTK_BOX (lyrics_box), GTK_WIDGET (self->lyrics_retry));

  self->lyrics_timed_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_valign (self->lyrics_timed_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (self->lyrics_timed_box, TRUE);
  gtk_widget_set_vexpand (self->lyrics_timed_box, TRUE);
  self->lyrics_previous = new_lyric_label ("dim-text");
  self->lyrics_current = new_lyric_label ("section-heading");
  self->lyrics_next = new_lyric_label ("dim-text");
  gtk_box_append (GTK_BOX (self->lyrics_timed_box),
                  GTK_WIDGET (self->lyrics_previous));
  gtk_box_append (GTK_BOX (self->lyrics_timed_box),
                  GTK_WIDGET (self->lyrics_current));
  gtk_box_append (GTK_BOX (self->lyrics_timed_box),
                  GTK_WIDGET (self->lyrics_next));
  /* Lyrics do not get to measure the artwork, track details or tabs. A
   * longer/wrapped line can increase both its width and height request; the
   * viewport reports neither natural dimension (and zero minimum content
   * height) to the parent, so the fixed upper section keeps its allocation
   * in a short, non-maximized window. GTK_POLICY_NEVER for vertical scrolling
   * defeats that boundary: GtkScrolledWindow then exposes the wrapped lines'
   * full minimum height. AUTOMATIC keeps the minimum fixed and scrolls only
   * when the available lyric space is truly too short. */
  self->lyrics_timed_scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (self->lyrics_timed_scroller, TRUE);
  gtk_widget_set_hexpand (self->lyrics_timed_scroller, TRUE);
  gtk_scrolled_window_set_min_content_width (
    GTK_SCROLLED_WINDOW (self->lyrics_timed_scroller), 0);
  gtk_scrolled_window_set_min_content_height (
    GTK_SCROLLED_WINDOW (self->lyrics_timed_scroller), 0);
  gtk_scrolled_window_set_propagate_natural_width (
    GTK_SCROLLED_WINDOW (self->lyrics_timed_scroller), FALSE);
  gtk_scrolled_window_set_propagate_natural_height (
    GTK_SCROLLED_WINDOW (self->lyrics_timed_scroller), FALSE);
  gtk_scrolled_window_set_policy (
    GTK_SCROLLED_WINDOW (self->lyrics_timed_scroller),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (
    GTK_SCROLLED_WINDOW (self->lyrics_timed_scroller), TRUE);
  gtk_scrolled_window_set_child (
    GTK_SCROLLED_WINDOW (self->lyrics_timed_scroller),
    self->lyrics_timed_box);
  gtk_widget_set_visible (self->lyrics_timed_scroller, FALSE);
  gtk_box_append (GTK_BOX (lyrics_box), self->lyrics_timed_scroller);

  self->lyrics_plain = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->lyrics_plain), "lyrics-plain");
  gtk_text_view_set_editable (self->lyrics_plain, FALSE);
  gtk_text_view_set_cursor_visible (self->lyrics_plain, FALSE);
  gtk_text_view_set_wrap_mode (self->lyrics_plain, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (self->lyrics_plain, 6);
  gtk_text_view_set_right_margin (self->lyrics_plain, 6);
  self->lyrics_plain_scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (self->lyrics_plain_scroller, TRUE);
  gtk_scrolled_window_set_min_content_width (
    GTK_SCROLLED_WINDOW (self->lyrics_plain_scroller), 0);
  gtk_scrolled_window_set_min_content_height (
    GTK_SCROLLED_WINDOW (self->lyrics_plain_scroller), 0);
  gtk_scrolled_window_set_propagate_natural_width (
    GTK_SCROLLED_WINDOW (self->lyrics_plain_scroller), FALSE);
  gtk_scrolled_window_set_propagate_natural_height (
    GTK_SCROLLED_WINDOW (self->lyrics_plain_scroller), FALSE);
  gtk_scrolled_window_set_policy (
    GTK_SCROLLED_WINDOW (self->lyrics_plain_scroller),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  spotifygtk_smooth_scroll_attach (
    GTK_SCROLLED_WINDOW (self->lyrics_plain_scroller),
    GTK_ORIENTATION_VERTICAL);
  gtk_scrolled_window_set_child (
    GTK_SCROLLED_WINDOW (self->lyrics_plain_scroller),
    GTK_WIDGET (self->lyrics_plain));
  gtk_widget_set_visible (self->lyrics_plain_scroller, FALSE);
  gtk_box_append (GTK_BOX (lyrics_box), self->lyrics_plain_scroller);

  self->lyrics_font_css = gtk_css_provider_new ();
  self->lyrics_font_display = g_object_ref (gdk_display_get_default ());
  gtk_style_context_add_provider_for_display (
    self->lyrics_font_display, GTK_STYLE_PROVIDER (self->lyrics_font_css),
    GTK_STYLE_PROVIDER_PRIORITY_USER);
  lyrics_apply_font_size (self, spotifygtk_settings_get_lyrics_font_size (
    spotifygtk_settings_get_default ()));
  gtk_stack_add_titled (self->content_stack, lyrics_box, "lyrics", "Lyrics");
  gtk_stack_set_visible_child_name (self->content_stack, "queue");
  /* StackSwitcher supplies active-state styling and keyboard accessibility.
   * Only opening Lyrics starts I/O; the default Queue view is inert. */
  g_signal_connect (self->content_stack, "notify::visible-child-name",
                    G_CALLBACK (on_lower_view_changed), self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->content_stack));
}

SpotifyGtkNowPlayingPanel *
spotifygtk_now_playing_panel_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_NOW_PLAYING_PANEL, NULL);
}

void
spotifygtk_now_playing_panel_set_track (SpotifyGtkNowPlayingPanel *self,
                                        const gchar *track_name,
                                        const gchar *artist,
                                        const gchar *album)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));

  gtk_label_set_text (self->track_label, track_name ? track_name : "");
  marquee_reset (self->track_label);

  g_autofree gchar *subtitle = g_strdup_printf ("%s • %s",
                                                artist ? artist : "",
                                                album ? album : "");
  gtk_label_set_text (self->artist_label, subtitle);
}

void
spotifygtk_now_playing_panel_set_album_art (SpotifyGtkNowPlayingPanel *self,
                                            const gchar *image_path)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));
  if (image_path && *image_path) {
    /* TODO: Load actual image */
  }
  (void) image_path;
}

void
spotifygtk_now_playing_panel_set_playing (SpotifyGtkNowPlayingPanel *self, gboolean is_playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));
  self->is_playing = is_playing;
}

static void
lyrics_show_status (SpotifyGtkNowPlayingPanel *self, const gchar *message)
{
  gtk_label_set_text (self->lyrics_status, message);
  gtk_widget_set_visible (GTK_WIDGET (self->lyrics_status), TRUE);
  gtk_widget_set_visible (self->lyrics_timed_scroller, FALSE);
  gtk_widget_set_visible (self->lyrics_plain_scroller, FALSE);
}

static void
lyrics_show_retry_status (SpotifyGtkNowPlayingPanel *self,
                          const gchar *message)
{
  lyrics_show_status (self, message);
  gtk_widget_set_visible (GTK_WIDGET (self->lyrics_retry), TRUE);
}

static void
lyrics_update_line (SpotifyGtkNowPlayingPanel *self)
{
  if (!self->lyrics_lines || self->lyrics_lines->len == 0)
    return;
  gint index = spotifygtk_lrc_active_line (self->lyrics_lines,
                                           self->lyrics_position_ms);
  if (index == self->lyrics_active_index)
    return; /* No GTK layout work on ordinary 250 ms position reports. */
  self->lyrics_active_index = index;

  guint count = self->lyrics_lines->len;
  const gchar *previous = index > 0
    ? ((SpotifyGtkLyricLine *) g_ptr_array_index (self->lyrics_lines,
                                                  index - 1))->text : "";
  const gchar *current = index >= 0
    ? ((SpotifyGtkLyricLine *) g_ptr_array_index (self->lyrics_lines,
                                                  index))->text : "";
  const gchar *next = (guint) (index + 1) < count
    ? ((SpotifyGtkLyricLine *) g_ptr_array_index (self->lyrics_lines,
                                                  index + 1))->text : "";
  gtk_label_set_text (self->lyrics_previous, previous);
  gtk_label_set_text (self->lyrics_current, current);
  gtk_label_set_text (self->lyrics_next, next);
  /* Layout must complete before the new line's bounds are meaningful. Only
   * one short-lived callback is allowed, and ordinary position polls do not
   * create one because the active index has not changed. */
  if (index >= 0 && !self->lyrics_center_tick_id &&
      gtk_widget_get_mapped (self->lyrics_timed_scroller)) {
    self->lyrics_center_tick_frames = 0;
    self->lyrics_center_tick_id = gtk_widget_add_tick_callback (
      self->lyrics_timed_scroller, lyrics_center_tick, self, NULL);
  }
}

static gboolean
lyrics_center_tick (GtkWidget *widget, GdkFrameClock *clock,
                    gpointer user_data)
{
  SpotifyGtkNowPlayingPanel *self = user_data;
  /* A tick runs before this frame's layout, so wait one completed frame. */
  if (self->lyrics_center_tick_frames++ == 0)
    return G_SOURCE_CONTINUE;
  self->lyrics_center_tick_id = 0;
  graphene_rect_t bounds;
  if (gtk_widget_compute_bounds (GTK_WIDGET (self->lyrics_current), widget,
                                 &bounds)) {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (
      GTK_SCROLLED_WINDOW (self->lyrics_timed_scroller));
    gdouble viewport = gtk_widget_get_height (widget);
    gdouble upper = gtk_adjustment_get_upper (adj);
    gdouble page = gtk_adjustment_get_page_size (adj);
    if (upper > page + 1 && viewport > 0) {
      gdouble center = bounds.origin.y + bounds.size.height / 2.0;
      gdouble desired = gtk_adjustment_get_value (adj) +
                        center - viewport / 2.0;
      gtk_adjustment_set_value (adj, CLAMP (desired,
        gtk_adjustment_get_lower (adj),
        MAX (gtk_adjustment_get_lower (adj), upper - page)));
    }
  }
  (void) clock;
  return G_SOURCE_REMOVE;
}

static void
lyrics_install (SpotifyGtkNowPlayingPanel *self, GPtrArray *lines,
                const gchar *plain)
{
  gtk_widget_set_visible (GTK_WIDGET (self->lyrics_retry), FALSE);
  g_clear_pointer (&self->lyrics_lines, g_ptr_array_unref);
  self->lyrics_lines = lines && lines->len > 0 ? g_ptr_array_ref (lines) : NULL;
  self->lyrics_active_index = -2; /* force a repaint after a track change */

  if (self->lyrics_lines) {
    gtk_widget_set_visible (GTK_WIDGET (self->lyrics_status), FALSE);
    gtk_widget_set_visible (self->lyrics_plain_scroller, FALSE);
    gtk_widget_set_visible (self->lyrics_timed_scroller, TRUE);
    lyrics_update_line (self);
  } else if (plain && *plain) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->lyrics_plain);
    gtk_text_buffer_set_text (buffer, plain, -1);
    gtk_widget_set_visible (GTK_WIDGET (self->lyrics_status), FALSE);
    gtk_widget_set_visible (self->lyrics_timed_scroller, FALSE);
    gtk_widget_set_visible (self->lyrics_plain_scroller, TRUE);
  } else {
    lyrics_show_status (self, "No lyrics available for this recording");
  }
}

static void
lyrics_cache_put (SpotifyGtkNowPlayingPanel *self, GPtrArray *lines,
                  const gchar *plain)
{
  if (!self->lyrics_uri)
    return;
  LyricsCacheEntry *entry = g_new0 (LyricsCacheEntry, 1);
  entry->lines = lines && lines->len > 0 ? g_ptr_array_ref (lines) : NULL;
  entry->plain = g_strdup (plain);
  entry->expires_us = g_get_monotonic_time () +
    (entry->lines || (entry->plain && *entry->plain)
      ? (gint64) 60 * 60 * G_USEC_PER_SEC
      : (gint64) 10 * 60 * G_USEC_PER_SEC);

  if (!g_hash_table_contains (self->lyrics_cache, self->lyrics_uri))
    g_queue_push_tail (&self->lyrics_cache_order, g_strdup (self->lyrics_uri));
  g_hash_table_replace (self->lyrics_cache, g_strdup (self->lyrics_uri), entry);
  while (g_queue_get_length (&self->lyrics_cache_order) > 16) {
    gchar *oldest = g_queue_pop_head (&self->lyrics_cache_order);
    g_hash_table_remove (self->lyrics_cache, oldest);
    g_free (oldest);
  }
}

static void
lyrics_online_finished (GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
  LyricsRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = soup_session_send_and_read_finish (
    SOUP_SESSION (source), result, &error);
  g_autoptr(SpotifyGtkNowPlayingPanel) self = g_weak_ref_get (&request->panel);
  if (!lyrics_request_current (request, self)) {
    lyrics_request_free (request);
    return;
  }
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      lyrics_show_retry_status (self, "Lyrics lookup failed; try again");
    lyrics_request_free (request);
    return;
  }

  guint status = soup_message_get_status (request->message);
  if (status == 429) {
    const gchar *retry = soup_message_headers_get_one (
      soup_message_get_response_headers (request->message), "Retry-After");
    guint64 seconds = retry ? g_ascii_strtoull (retry, NULL, 10) : 60;
    seconds = CLAMP (seconds, 1, 3600);
    self->lyrics_retry_until_us = g_get_monotonic_time () +
                                   (gint64) seconds * G_USEC_PER_SEC;
    lyrics_show_retry_status (self, "Lyrics service is busy; try later");
    lyrics_request_free (request);
    return;
  }
  if (status == SOUP_STATUS_NOT_FOUND) {
    lyrics_cache_put (self, NULL, NULL); /* short-lived negative result */
    lyrics_show_status (self, "No lyrics available for this recording");
    lyrics_request_free (request);
    return;
  }
  if (status != SOUP_STATUS_OK || !bytes) {
    lyrics_show_retry_status (self, "Lyrics lookup failed; try again");
    lyrics_request_free (request);
    return;
  }

  gsize length = 0;
  const gchar *data = g_bytes_get_data (bytes, &length);
  if (length > 256 * 1024) {
    lyrics_show_status (self, "Lyrics response was too large");
    lyrics_request_free (request);
    return;
  }
  g_autoptr(JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, (gssize) length, NULL) ||
      !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser))) {
    lyrics_show_status (self, "Lyrics response was unreadable");
    lyrics_request_free (request);
    return;
  }
  JsonObject *object = json_node_get_object (json_parser_get_root (parser));
  if (json_object_has_member (object, "duration") &&
      JSON_NODE_HOLDS_VALUE (json_object_get_member (object, "duration"))) {
    gint64 reported = json_object_get_int_member (object, "duration");
    gint64 expected = (self->lyrics_duration_ms + 500) / 1000;
    if (reported > 0 && llabs (reported - expected) > 2) {
      lyrics_show_status (self, "Lyrics match a different recording");
      lyrics_request_free (request);
      return;
    }
  }
  const gchar *synced = json_object_has_member (object, "syncedLyrics") &&
                        JSON_NODE_HOLDS_VALUE (json_object_get_member (object,
                                                                       "syncedLyrics"))
    ? json_object_get_string_member (object, "syncedLyrics") : NULL;
  const gchar *plain = json_object_has_member (object, "plainLyrics") &&
                       JSON_NODE_HOLDS_VALUE (json_object_get_member (object,
                                                                      "plainLyrics"))
    ? json_object_get_string_member (object, "plainLyrics") : NULL;
  g_autoptr(GPtrArray) lines = spotifygtk_lrc_parse (synced);
  lyrics_cache_put (self, lines, plain);
  lyrics_install (self, lines, plain);
  lyrics_request_free (request);
}

static void
lyrics_start_online (LyricsRequest *request)
{
  g_autoptr(SpotifyGtkNowPlayingPanel) self = g_weak_ref_get (&request->panel);
  if (!lyrics_request_current (request, self)) {
    lyrics_request_free (request);
    return;
  }
  if (!spotifygtk_settings_get_online_lyrics (
        spotifygtk_settings_get_default ())) {
    lyrics_show_status (self, "Add a local .lrc file or enable Online lyrics in Settings");
    lyrics_request_free (request);
    return;
  }
  if (g_get_monotonic_time () < self->lyrics_retry_until_us) {
    lyrics_show_retry_status (self, "Lyrics service is busy; try later");
    lyrics_request_free (request);
    return;
  }
  LyricsCacheEntry *cached = g_hash_table_lookup (self->lyrics_cache,
                                                  self->lyrics_uri);
  if (cached && cached->expires_us > g_get_monotonic_time ()) {
    lyrics_install (self, cached->lines, cached->plain);
    lyrics_request_free (request);
    return;
  }
  if (!self->lyrics_title || !*self->lyrics_title ||
      !self->lyrics_artist || !*self->lyrics_artist ||
      self->lyrics_duration_ms <= 0) {
    lyrics_show_status (self, "This song lacks the metadata needed to find lyrics");
    lyrics_request_free (request);
    return;
  }

  g_autofree gchar *primary_artist = g_strdup (self->lyrics_artist);
  gchar *separator = strstr (primary_artist, ", ");
  if (separator)
    *separator = '\0';
  g_autofree gchar *title = g_uri_escape_string (self->lyrics_title, NULL, FALSE);
  g_autofree gchar *artist = g_uri_escape_string (primary_artist, NULL, FALSE);
  g_autofree gchar *album = g_uri_escape_string (
    self->lyrics_album ? self->lyrics_album : "", NULL, FALSE);
  gint64 duration = CLAMP ((self->lyrics_duration_ms + 500) / 1000, 1, 3600);
  g_autofree gchar *url = g_strdup_printf (
    "https://lrclib.net/api/get?track_name=%s&artist_name=%s&album_name=%s"
    "&duration=%" G_GINT64_FORMAT, title, artist, album, duration);
  g_autoptr(SoupMessage) message = soup_message_new (SOUP_METHOD_GET, url);
  if (!message) {
    lyrics_show_status (self, "Lyrics lookup could not start");
    lyrics_request_free (request);
    return;
  }
  soup_message_headers_replace (soup_message_get_request_headers (message),
    "User-Agent", "SpotifyGTK/0.1 (https://github.com/martiancomputer/SpotifyGTK)");
  if (!self->lyrics_session)
    self->lyrics_session = soup_session_new ();
  request->message = g_object_ref (message);
  lyrics_show_status (self, "Finding lyrics…");
  soup_session_send_and_read_async (self->lyrics_session, message,
    G_PRIORITY_DEFAULT, self->lyrics_cancellable,
    lyrics_online_finished, request);
}

static void
lyrics_local_finished (GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
  LyricsRequest *request = user_data;
  gchar *contents = NULL;
  gsize length = 0;
  g_autoptr(GError) error = NULL;
  gboolean found = g_file_load_contents_finish (G_FILE (source), result,
                                                &contents, &length, NULL, &error);
  g_autofree gchar *text = contents;
  g_autoptr(SpotifyGtkNowPlayingPanel) self = g_weak_ref_get (&request->panel);
  if (!lyrics_request_current (request, self)) {
    lyrics_request_free (request);
    return;
  }
  if (found && length <= 256 * 1024 && g_utf8_validate (text, length, NULL)) {
    g_autoptr(GPtrArray) lines = spotifygtk_lrc_parse (text);
    if (lines->len > 0 || (text && *text)) {
      lyrics_install (self, lines, lines->len == 0 ? text : NULL);
      lyrics_request_free (request);
      return;
    }
  }
  /* Missing local file is expected. An invalid one can still be replaced by
   * the online source; no synchronous file I/O or repeated reads per frame. */
  lyrics_start_online (request);
}

static void
lyrics_begin_load (SpotifyGtkNowPlayingPanel *self)
{
  gtk_widget_set_visible (GTK_WIDGET (self->lyrics_retry), FALSE);
  self->lyrics_requested = TRUE;
  self->lyrics_generation++;
  if (self->lyrics_cancellable)
    g_cancellable_cancel (self->lyrics_cancellable);
  g_clear_object (&self->lyrics_cancellable);
  g_clear_pointer (&self->lyrics_lines, g_ptr_array_unref);
  self->lyrics_active_index = -2;
  g_autofree gchar *share_url = spotifygtk_track_share_url (self->lyrics_uri);
  if (!share_url) {
    lyrics_show_status (self, "Play a Spotify song to see lyrics");
    return;
  }
  self->lyrics_cancellable = g_cancellable_new ();
  const gchar *id = strrchr (self->lyrics_uri, ':') + 1;
  g_autofree gchar *filename = g_strconcat (id, ".lrc", NULL);
  g_autofree gchar *path = g_build_filename (
    g_get_user_data_dir (), "spotifygtk", "lyrics", filename, NULL);
  g_autoptr(GFile) file = g_file_new_for_path (path);
  lyrics_show_status (self, "Looking for local lyrics…");
  g_file_load_contents_async (file, self->lyrics_cancellable,
                              lyrics_local_finished, lyrics_request_new (self));
}

void
spotifygtk_now_playing_panel_set_lyrics_track (SpotifyGtkNowPlayingPanel *self,
                                               const SpotifyNativeTrack *track)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));
  const gchar *uri = track ? track->uri : NULL;
  if (g_strcmp0 (self->lyrics_uri, uri) == 0)
    return;
  g_free (self->lyrics_uri);
  g_free (self->lyrics_title);
  g_free (self->lyrics_artist);
  g_free (self->lyrics_album);
  self->lyrics_uri = g_strdup (uri);
  self->lyrics_title = g_strdup (track ? track->name : NULL);
  self->lyrics_artist = g_strdup (track ? track->artists : NULL);
  self->lyrics_album = g_strdup (track ? track->album : NULL);
  self->lyrics_duration_ms = track ? track->duration_ms : 0;
  self->lyrics_position_ms = 0;
  self->lyrics_requested = FALSE;
  self->lyrics_generation++;
  if (self->lyrics_cancellable)
    g_cancellable_cancel (self->lyrics_cancellable);
  g_clear_object (&self->lyrics_cancellable);
  g_clear_pointer (&self->lyrics_lines, g_ptr_array_unref);
  self->lyrics_active_index = -2;
  if (g_strcmp0 (gtk_stack_get_visible_child_name (self->content_stack),
                 "lyrics") == 0)
    lyrics_begin_load (self);
  else
    lyrics_show_status (self, "Open Lyrics to load this song");
}

static void
on_lyrics_setting_changed (SpotifyGtkSettings *settings, gpointer user_data)
{
  SpotifyGtkNowPlayingPanel *self = user_data;
  guint font_size = spotifygtk_settings_get_lyrics_font_size (settings);
  if (font_size != self->lyrics_font_size_seen)
    lyrics_apply_font_size (self, font_size);
  gboolean enabled = spotifygtk_settings_get_online_lyrics (settings);
  if (enabled == self->online_lyrics_seen)
    return;
  self->online_lyrics_seen = enabled;
  if (self->lyrics_uri &&
      g_strcmp0 (gtk_stack_get_visible_child_name (self->content_stack),
                 "lyrics") == 0)
    lyrics_begin_load (self);
  else
    {
      self->lyrics_requested = FALSE;
      self->lyrics_generation++;
      if (self->lyrics_cancellable)
        g_cancellable_cancel (self->lyrics_cancellable);
      g_clear_object (&self->lyrics_cancellable);
    }
}

void
spotifygtk_now_playing_panel_set_progress (SpotifyGtkNowPlayingPanel *self,
                                           gint64 position_ms,
                                           gint64 duration_ms)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));
  self->lyrics_position_ms = position_ms;
  if (g_strcmp0 (gtk_stack_get_visible_child_name (self->content_stack),
                 "lyrics") == 0)
    lyrics_update_line (self);
  (void) duration_ms;
}

void
spotifygtk_now_playing_panel_set_queue (SpotifyGtkNowPlayingPanel *self, JsonArray *tracks)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));

  /* Clear existing */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->queue_list))))
    gtk_list_box_remove (self->queue_list, child);

  if (!tracks) return;

  for (guint i = 0; i < json_array_get_length (tracks); i++) {
    JsonObject *track = json_array_get_object_element (tracks, i);
    const gchar *name = json_object_get_string_member_with_default (track, "name", "");

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_widget_add_css_class (row, "list-row");

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_widget_set_margin_top (box, 12);
    gtk_widget_set_margin_bottom (box, 12);

    GtkWidget *label = gtk_label_new (name);
    gtk_widget_add_css_class (label, "normal-text");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (box), label);

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_list_box_append (self->queue_list, row);
  }
}

void
spotifygtk_now_playing_panel_set_native_queue (SpotifyGtkNowPlayingPanel *self,
                                               GPtrArray *tracks)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->queue_list))))
    gtk_list_box_remove (self->queue_list, child);

  guint n = tracks ? tracks->len : 0;
  gtk_widget_set_visible (GTK_WIDGET (self->queue_heading), n > 0);

  for (guint i = 0; i < n; i++) {
    const SpotifyNativeTrack *track = g_ptr_array_index (tracks, i);
    if (!track)
      continue;

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_widget_add_css_class (row, "list-row");
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_widget_set_margin_top (box, 8);
    gtk_widget_set_margin_bottom (box, 8);

    GtkWidget *name = gtk_label_new (track->name ? track->name : "Unknown track");
    gtk_widget_add_css_class (name, "normal-text");
    gtk_label_set_xalign (GTK_LABEL (name), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (box), name);

    if (track->artists && *track->artists) {
      GtkWidget *artist = gtk_label_new (track->artists);
      gtk_widget_add_css_class (artist, "dim-text");
      gtk_label_set_xalign (GTK_LABEL (artist), 0.0);
      gtk_label_set_ellipsize (GTK_LABEL (artist), PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (box), artist);
    }

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_list_box_append (self->queue_list, row);
  }
}

static void
on_cover_loaded_spotifygtk_now_playing_panel (GdkTexture *texture, gpointer user_data)
{
  SpotifyGtkNowPlayingPanel *self = user_data;

  if (texture) {
    gtk_picture_set_paintable (self->album_pic, GDK_PAINTABLE (texture));
    gtk_widget_set_visible (GTK_WIDGET (self->album_pic), TRUE);
  } else {
    gtk_picture_set_paintable (self->album_pic, NULL);
    gtk_widget_set_visible (GTK_WIDGET (self->album_pic), FALSE);
  }
}

void
spotifygtk_now_playing_panel_set_cover (SpotifyGtkNowPlayingPanel *self, const gchar *cover_id)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));

  /* No cancellable: there is exactly one of these widgets, and a late cover
   * can only ever belong to the track it was asked for or be superseded by
   * the next call, which overwrites it anyway. */
  spotifygtk_cover_load_playback (
    cover_id, ART_DECODE_PX, NULL,
    on_cover_loaded_spotifygtk_now_playing_panel, self);
}

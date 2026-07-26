/*
 * now_playing_panel.c — Right-side Now Playing panel implementation.
 */

#include "now_playing_panel.h"

#include <math.h>

/* Fixed cover edge; the panel is ~300px wide, so this leaves a margin. */
#define ART_SIZE 260
#include "cover_loader.h"

struct _SpotifyGtkNowPlayingPanel {
  GtkBox parent_instance;

  GtkImage   *album_art;    /* placeholder icon, shown when no cover */
  GtkPicture *album_pic;    /* the cover, scaled to fill */
  GtkLabel *track_label;
  GtkLabel *artist_label;
  GtkLabel *queue_heading;    /* "Next Up"; hidden when the queue is empty */
  GtkListBox *queue_list;
  GtkWidget  *art_section;   /* artwork + track info; what collapses */
  GtkButton  *collapse_btn;
  gboolean    collapsed;

  gboolean is_playing;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkNowPlayingPanel, spotifygtk_now_playing_panel, GTK_TYPE_BOX)

enum { COLLAPSE_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
spotifygtk_now_playing_panel_dispose (GObject *object)
{
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
  gint64             start_us;
} Marquee;

static gboolean
marquee_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  Marquee *m = user_data;
  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (m->scroller);
  gdouble span = gtk_adjustment_get_upper (hadj) - gtk_adjustment_get_page_size (hadj);

  if (span <= 1.0) {                       /* title fits: hold at the start */
    gtk_adjustment_set_value (hadj, 0.0);
    m->start_us = 0;
    return G_SOURCE_CONTINUE;
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
  g_object_set_data (G_OBJECT (label), "marquee", m);
  gtk_widget_add_tick_callback (label, marquee_tick, m, g_free);

  *out_label = GTK_LABEL (label);
  return scroller;
}

static void
marquee_reset (GtkLabel *label)
{
  Marquee *m = g_object_get_data (G_OBJECT (label), "marquee");
  if (m)
    m->start_us = 0;   /* next tick restarts from the left */
}

static void
spotifygtk_now_playing_panel_init (SpotifyGtkNowPlayingPanel *self)
{
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

  /* A fixed square, centred, that does NOT expand. GtkPicture reports its
   * paintable's intrinsic size as its natural size, so without a hard
   * size_request a large cover made the art (and with it the whole right
   * pane) grow, and covers of different sizes reflowed the panel on every
   * track change. Locking the box means the art is always ART_SIZE regardless
   * of the cover, and it can never drive the panel width. */
  gtk_widget_set_size_request (art_overlay, ART_SIZE, ART_SIZE);
  gtk_widget_set_halign (art_overlay, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (art_overlay, FALSE);
  gtk_widget_set_valign (art_overlay, GTK_ALIGN_START);
  gtk_widget_set_margin_top (art_overlay, 8);
  gtk_box_append (GTK_BOX (self->art_section), art_overlay);

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
  gtk_box_append (GTK_BOX (self), self->art_section);

  /* Queue list, under a "Next Up" heading that hides itself when empty so an
   * idle panel is not left with a dangling label over nothing. */
  self->queue_heading = GTK_LABEL (gtk_label_new ("Next Up"));
  gtk_widget_add_css_class (GTK_WIDGET (self->queue_heading), "dim-text");
  gtk_label_set_xalign (self->queue_heading, 0.0);
  gtk_widget_set_margin_start (GTK_WIDGET (self->queue_heading), 20);
  gtk_widget_set_margin_end (GTK_WIDGET (self->queue_heading), 16);
  gtk_widget_set_margin_top (GTK_WIDGET (self->queue_heading), 16);
  gtk_widget_set_visible (GTK_WIDGET (self->queue_heading), FALSE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->queue_heading));

  GtkWidget *queue_scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (queue_scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (queue_scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (queue_scroller), FALSE);

  self->queue_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->queue_list, GTK_SELECTION_NONE);
  gtk_widget_set_margin_top (GTK_WIDGET (self->queue_list), 8);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (queue_scroller),
                                 GTK_WIDGET (self->queue_list));
  gtk_box_append (GTK_BOX (self), queue_scroller);
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

void
spotifygtk_now_playing_panel_set_progress (SpotifyGtkNowPlayingPanel *self,
                                           gint64 position_ms,
                                           gint64 duration_ms)
{
  g_return_if_fail (SPOTIFYGTK_IS_NOW_PLAYING_PANEL (self));
  /* Progress is shown in playback bar, not here in mockup */
  (void) self; (void) position_ms; (void) duration_ms;
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
  spotifygtk_cover_load (cover_id, 420, NULL, on_cover_loaded_spotifygtk_now_playing_panel, self);
}

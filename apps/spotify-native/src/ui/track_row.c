/*
 * track_row.c — Reusable track list row widget.
 */

#include "track_row.h"

/* Width of the like slot, so the hearts line up across rows. */
#define ROW_LIKE_WIDTH 22
#define ROW_LIKE_PX    14

/*
 * Natural-width caps for the text labels. Not display limits -- the labels
 * still expand into whatever space the row has -- but a bound on what they ask
 * for, so a long title cannot drag the window wider.
 */
#define ROW_TITLE_MAX_CHARS 48
#define ROW_META_MAX_CHARS  28

/* Wide enough for the longest duration ("10:05") and for EQ_WIDTH, so the two
 * states are the same size and the column does not twitch when one replaces
 * the other. */
#define ROW_STATUS_WIDTH 66

/* Pulls the column clear of the scrollbar gutter so it lines up with the rest
 * of the content rather than the very edge of the viewport. */
#define ROW_STATUS_MARGIN_END 10

#include <string.h>
#include <math.h>

#include "cover_loader.h"

struct _SpotifyGtkTrackRow {
  GtkListBoxRow parent_instance;

  GtkBox *root_box;
  GtkImage *album_art;
  GtkLabel *track_num;
  GtkLabel *title_label;
  GtkLabel *artist_label;
  GtkLabel *album_label;
  GtkWidget *status_slot;   /* holds duration OR the equaliser */
  GtkLabel *duration_label;
  GtkWidget *like_icon;     /* indicator only, not a control */
  gboolean   liked;
  GtkButton *play_btn;
  GtkBox *action_box;

  /* Now-playing equaliser: three bars beside the duration. */
  GtkWidget *eq_area;
  guint      eq_tick_id;

  /* Cancelled when the row is reused or destroyed, so a slow cover cannot
   * land on a row that now shows a different track. */
  GCancellable *cover_cancellable;
  /* The cover this row wants, kept so a load skipped during a scroll can be
   * reissued once the list settles. */
  gchar        *pending_cover_id;
  gint          row_number;      /* 0 in an unnumbered list */
  gboolean      cover_shown;

  gboolean show_album;
  gboolean show_artists;
  gboolean is_playing;
  gboolean is_paused;

  gchar *track_uri;
  gchar *track_id;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkTrackRow, spotifygtk_track_row, GTK_TYPE_BOX)

enum {
  PLAY_CLICKED,
  QUEUE_CLICKED,
  ARTIST_ACTIVATED,
  ALBUM_ACTIVATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
spotifygtk_track_row_dispose (GObject *object)
{
  SpotifyGtkTrackRow *self = SPOTIFYGTK_TRACK_ROW (object);
  if (self->cover_cancellable) {
    g_cancellable_cancel (self->cover_cancellable);
    g_clear_object (&self->cover_cancellable);
  }
  g_clear_pointer (&self->pending_cover_id, g_free);
  if (self->eq_tick_id != 0 && self->eq_area) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self->eq_area), self->eq_tick_id);
    self->eq_tick_id = 0;
  }
  g_clear_pointer (&self->track_uri, g_free);
  g_clear_pointer (&self->track_id, g_free);
  G_OBJECT_CLASS (spotifygtk_track_row_parent_class)->dispose (object);
}

static void
spotifygtk_track_row_class_init (SpotifyGtkTrackRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = spotifygtk_track_row_dispose;

  signals[PLAY_CLICKED] = g_signal_new ("play-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[QUEUE_CLICKED] = g_signal_new ("queue-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[ARTIST_ACTIVATED] = g_signal_new ("artist-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[ALBUM_ACTIVATED] = g_signal_new ("album-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

/* === Now-playing equaliser === */

#define EQ_BARS       3
#define EQ_BAR_WIDTH  2
#define EQ_BAR_GAP    2
#define EQ_HEIGHT    11
#define EQ_WIDTH     (EQ_BARS * EQ_BAR_WIDTH + (EQ_BARS - 1) * EQ_BAR_GAP)

/* Each bar runs on its own phase so they never move in lockstep. Periods are
 * deliberately not integer multiples of one another, so the group does not
 * visibly repeat on a short cycle. */
static const gdouble EQ_PERIODS[EQ_BARS] = { 0.62, 0.43, 0.81 };
static const gdouble EQ_PHASES[EQ_BARS]  = { 0.0,  0.5,  0.25 };

static void
eq_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
  gdouble seconds = *(gdouble *) user_data;

  /* Accent green, matching .eq-bar in the stylesheet. Drawn rather than
   * styled because the heights change per frame. */
  cairo_set_source_rgb (cr, 0.114, 0.725, 0.329);

  for (int i = 0; i < EQ_BARS; i++) {
    gdouble t = seconds / EQ_PERIODS[i] + EQ_PHASES[i];
    /* sin -> 0..1, then 25%..100% of the available height. */
    gdouble level = 0.25 + 0.75 * (0.5 + 0.5 * sin (t * 2.0 * G_PI));
    gdouble h = level * height;
    gdouble x = i * (EQ_BAR_WIDTH + EQ_BAR_GAP);

    cairo_rectangle (cr, x, height - h, EQ_BAR_WIDTH, h);
  }
  cairo_fill (cr);

  (void) area; (void) width;
}

static gboolean
eq_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  SpotifyGtkTrackRow *self = user_data;

  /* Frame-clock driven, so it runs at the monitor's rate and GTK throttles
   * it automatically when the row is not being drawn. */
  gint64 us = gdk_frame_clock_get_frame_time (clock);
  gdouble *seconds = g_object_get_data (G_OBJECT (self->eq_area), "eq-seconds");
  if (seconds)
    *seconds = (gdouble) us / (gdouble) G_USEC_PER_SEC;

  gtk_widget_queue_draw (self->eq_area);
  (void) widget;
  return G_SOURCE_CONTINUE;
}

static void
eq_set_running (SpotifyGtkTrackRow *self, gboolean running)
{
  if (running && self->eq_tick_id == 0) {
    self->eq_tick_id = gtk_widget_add_tick_callback (GTK_WIDGET (self->eq_area),
                                                     eq_tick, self, NULL);
  } else if (!running && self->eq_tick_id != 0) {
    /* Stop outright rather than leaving a callback running for a row that is
     * no longer playing -- with a long list that would be one animation per
     * row forever. */
    gtk_widget_remove_tick_callback (GTK_WIDGET (self->eq_area), self->eq_tick_id);
    self->eq_tick_id = 0;
  }
  gtk_widget_set_visible (self->eq_area, running);
}

static void
on_row_cover_loaded (GdkTexture *texture, gpointer user_data)
{
  SpotifyGtkTrackRow *self = user_data;

  if (!texture)
    return;   /* keep the placeholder icon; retry_cover() may come back for it */

  self->cover_shown = TRUE;
  gtk_image_set_from_paintable (self->album_art, GDK_PAINTABLE (texture));
  gtk_widget_set_visible (GTK_WIDGET (self->album_art), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->track_num), FALSE);
}

/*
 * Liked state. Shown only when liked -- a green heart means saved, nothing
 * means not, which is all a list needs to say. The action lives in the context
 * menu, where it can name what it will do rather than relying on the reader to
 * infer it from a filled or hollow shape.
 *
 * No logic behind this yet: nothing calls it, so every row reads unliked until
 * a collection read is wired up.
 */
void
spotifygtk_track_row_set_liked (SpotifyGtkTrackRow *self, gboolean liked)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));
  self->liked = liked;
  gtk_widget_set_opacity (self->like_icon, liked ? 1.0 : 0.0);
}

gboolean
spotifygtk_track_row_get_liked (SpotifyGtkTrackRow *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self), FALSE);
  return self->liked;
}

/* Hidden on the Liked Songs page, where every row is liked by definition and a
 * column of identical hearts says nothing. Keeps its width so durations stay
 * put. */
void
spotifygtk_track_row_set_like_visible (SpotifyGtkTrackRow *self, gboolean visible)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));
  gtk_widget_set_visible (self->like_icon, visible);
}

void
spotifygtk_track_row_retry_cover (SpotifyGtkTrackRow *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));

  if (self->cover_shown || !self->pending_cover_id)
    return;

  spotifygtk_cover_load_deferrable (self->pending_cover_id, 96, self->cover_cancellable,
                                    on_row_cover_loaded, self);
}

/* Rows are rebuilt per listing, but a row can be re-set before its cover
 * arrives; drop the old request first. */
static void
row_request_cover (SpotifyGtkTrackRow *self, const gchar *cover_id)
{
  if (self->cover_cancellable) {
    g_cancellable_cancel (self->cover_cancellable);
    g_clear_object (&self->cover_cancellable);
  }

  if (!cover_id || !*cover_id)
    return;

  self->cover_cancellable = g_cancellable_new ();
  g_free (self->pending_cover_id);
  self->pending_cover_id = g_strdup (cover_id);

  /* A recycled row still shows the previous track's cover; this is the one
   * place that knows the artwork has just gone stale. */
  self->cover_shown = FALSE;
  gtk_widget_set_visible (GTK_WIDGET (self->album_art), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->track_num), self->row_number > 0);
  spotifygtk_cover_load_deferrable (cover_id, 96, self->cover_cancellable,
                                    on_row_cover_loaded, self);
}

static void
on_play_btn_clicked (GtkButton *btn, gpointer user_data)
{
  SpotifyGtkTrackRow *self = user_data;
  g_signal_emit (self, signals[PLAY_CLICKED], 0);
  (void) btn;
}

static void
on_row_hover_enter (GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer user_data)
{
  SpotifyGtkTrackRow *self = user_data;
  gtk_widget_set_visible (GTK_WIDGET (self->action_box), TRUE);
  (void) ctrl; (void) x; (void) y;
}

static void
on_row_hover_leave (GtkEventControllerMotion *ctrl, gpointer user_data)
{
  SpotifyGtkTrackRow *self = user_data;
  gtk_widget_set_visible (GTK_WIDGET (self->action_box), FALSE);
  (void) ctrl;
}

static void
spotifygtk_track_row_init (SpotifyGtkTrackRow *self)
{
  self->show_album = TRUE;
  self->show_artists = TRUE;

  self->root_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12));
  gtk_widget_set_margin_start (GTK_WIDGET (self->root_box), 8);
  gtk_widget_set_margin_end (GTK_WIDGET (self->root_box), 8);
  gtk_widget_set_margin_top (GTK_WIDGET (self->root_box), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->root_box), 6);

  /* Track number / album art / playing indicator */
  self->track_num = GTK_LABEL (gtk_label_new (""));
  gtk_widget_set_size_request (GTK_WIDGET (self->track_num), 24, -1);
  gtk_widget_add_css_class (GTK_WIDGET (self->track_num), "row-number");
  gtk_label_set_xalign (self->track_num, 1.0);
  gtk_box_append (GTK_BOX (self->root_box), GTK_WIDGET (self->track_num));

  self->album_art = GTK_IMAGE (gtk_image_new_from_icon_name ("audio-x-generic-symbolic"));
  gtk_image_set_pixel_size (self->album_art, 40);
  gtk_widget_add_css_class (GTK_WIDGET (self->album_art), "card");
  gtk_widget_set_visible (GTK_WIDGET (self->album_art), FALSE);
  gtk_box_append (GTK_BOX (self->root_box), GTK_WIDGET (self->album_art));

  /* Track info */
  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (info, TRUE);

  self->title_label = GTK_LABEL (gtk_label_new ("Track"));
  gtk_label_set_xalign (self->title_label, 0.0);
  gtk_label_set_ellipsize (self->title_label, PANGO_ELLIPSIZE_END);
  /*
   * Ellipsizing alone does not bound a label: it changes what is drawn when
   * space is short, while the *natural* width request stays the full text.
   * GtkListView asks rows for their natural width, so one long title made the
   * whole list -- and then the window -- grow to fit it. max_width_chars caps
   * the request; hexpand still lets the label use whatever room exists, so
   * wide windows are unaffected and only over-long text ellipsizes.
   */
  gtk_label_set_max_width_chars (self->title_label, ROW_TITLE_MAX_CHARS);
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->title_label));

  GtkWidget *meta_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->artist_label = GTK_LABEL (gtk_label_new ("Artist"));
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "caption");
  gtk_label_set_xalign (self->artist_label, 0.0);
  gtk_label_set_ellipsize (self->artist_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (self->artist_label, ROW_META_MAX_CHARS);
  gtk_box_append (GTK_BOX (meta_row), GTK_WIDGET (self->artist_label));

  self->album_label = GTK_LABEL (gtk_label_new ("Album"));
  gtk_widget_add_css_class (GTK_WIDGET (self->album_label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->album_label), "caption");
  gtk_label_set_xalign (self->album_label, 0.0);
  gtk_label_set_ellipsize (self->album_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (self->album_label, ROW_META_MAX_CHARS);
  gtk_box_append (GTK_BOX (meta_row), GTK_WIDGET (self->album_label));

  gtk_box_append (GTK_BOX (info), meta_row);
  gtk_box_append (GTK_BOX (self->root_box), info);

  /*
   * Duration and the now-playing equaliser occupy the same slot, one at a
   * time. Side by side they made the playing row wider than every other, so
   * its indicator sat outside the column the durations line up in. Swapping
   * them in place keeps that column straight whichever row is playing.
   */
  self->status_slot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (self->status_slot, ROW_STATUS_WIDTH, -1);
  gtk_widget_set_halign (self->status_slot, GTK_ALIGN_END);
  gtk_widget_set_margin_end (self->status_slot, ROW_STATUS_MARGIN_END);
  gtk_widget_set_valign (self->status_slot, GTK_ALIGN_CENTER);

  /*
   * Liked indicator, inside the status slot rather than in the row flow.
   *
   * As a free-standing button it sat after the info box, whose width follows
   * the title and artist text, so the hearts landed at a different x on every
   * row. Anchored to the duration -- which is a fixed-width column -- text
   * length cannot move it at all.
   *
   * Its space is reserved whether or not it is showing, so liking a track does
   * not shift that row's duration relative to its neighbours.
   */
  self->like_icon = gtk_image_new_from_icon_name ("spotifygtk-heart-filled-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->like_icon), ROW_LIKE_PX);
  gtk_widget_add_css_class (self->like_icon, "like-active");
  gtk_widget_set_valign (self->like_icon, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request (self->like_icon, ROW_LIKE_WIDTH, -1);
  gtk_widget_set_opacity (self->like_icon, 0.0);   /* shown only when liked */
  gtk_box_append (GTK_BOX (self->status_slot), self->like_icon);

  self->duration_label = GTK_LABEL (gtk_label_new ("0:00"));
  gtk_widget_add_css_class (GTK_WIDGET (self->duration_label), "row-duration");
  gtk_label_set_xalign (self->duration_label, 1.0);
  gtk_widget_set_hexpand (GTK_WIDGET (self->duration_label), TRUE);
  gtk_box_append (GTK_BOX (self->status_slot), GTK_WIDGET (self->duration_label));

  /* Equaliser, immediately right of the duration. Hidden unless this row is
   * the one playing. */
  self->eq_area = gtk_drawing_area_new ();
  gtk_widget_set_size_request (self->eq_area, EQ_WIDTH, EQ_HEIGHT);
  gtk_widget_set_valign (self->eq_area, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->eq_area, FALSE);
  {
    gdouble *seconds = g_new0 (gdouble, 1);
    g_object_set_data_full (G_OBJECT (self->eq_area), "eq-seconds", seconds, g_free);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->eq_area),
                                    eq_draw, seconds, NULL);
  }
  /* Right-aligned like the duration text it replaces, so the indicator lands
   * in the same column rather than at the left of the slot. */
  gtk_widget_set_halign (self->eq_area, GTK_ALIGN_END);
  gtk_widget_set_hexpand (self->eq_area, TRUE);
  gtk_box_append (GTK_BOX (self->status_slot), self->eq_area);
  gtk_box_append (GTK_BOX (self->root_box), self->status_slot);

  /* Actions (hidden by default, shown on hover) */
  self->action_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4));
  self->play_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("media-playback-start-symbolic"));
  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "flat");
  gtk_box_append (GTK_BOX (self->action_box), GTK_WIDGET (self->play_btn));
  gtk_widget_set_visible (GTK_WIDGET (self->action_box), FALSE);
  gtk_box_append (GTK_BOX (self->root_box), GTK_WIDGET (self->action_box));

  g_signal_connect (self->play_btn, "clicked", G_CALLBACK (on_play_btn_clicked), self);

  /* Reveal the action buttons only while the pointer is over the row. */
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter", G_CALLBACK (on_row_hover_enter), self);
  g_signal_connect (motion, "leave", G_CALLBACK (on_row_hover_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->root_box));
}

SpotifyGtkTrackRow *
spotifygtk_track_row_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_TRACK_ROW, NULL);
}

void
spotifygtk_track_row_set_track (SpotifyGtkTrackRow *self, JsonObject *track_data, gint track_number)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));
  if (!track_data) return;

  const gchar *name = json_object_get_string_member_with_default (track_data, "name", "");
  const gchar *uri = json_object_get_string_member_with_default (track_data, "uri", "");
  const gchar *id = json_object_get_string_member_with_default (track_data, "id", "");
  gint64 duration_ms = json_object_has_member (track_data, "duration_ms") ?
    json_object_get_int_member (track_data, "duration_ms") : 0;

  g_free (self->track_uri);
  g_free (self->track_id);
  self->track_uri = g_strdup (uri);
  self->track_id = g_strdup (id);

  gtk_label_set_text (self->title_label, name);

  /* Track number */
  self->row_number = track_number;

  if (track_number > 0) {
    g_autofree gchar *num_text = g_strdup_printf ("%d", track_number);
    gtk_label_set_text (self->track_num, num_text);
    /*
     * The number shows only while there is no cover to show instead, and this
     * says nothing about the artwork either way.
     *
     * It used to hide album_art outright, which was harmless while a number
     * was set once per bind and became a bug the moment numbering started
     * following notify::position: every shift of a row re-hid art that had
     * already loaded. On a 5,000-row list positions shift constantly, so
     * covers were fetched, decoded, shown and hidden again -- indistinguishable
     * from never loading. Short lists barely move, which is why albums and
     * playlists never showed it.
     */
    gtk_widget_set_visible (GTK_WIDGET (self->track_num), !self->cover_shown);
  } else {
    /* No number: hide the column entirely rather than leaving a blank
     * 24px indent, which made unnumbered lists sit further right than
     * numbered ones. */
    gtk_widget_set_visible (GTK_WIDGET (self->track_num), FALSE);
  }

  /* Artist */
  JsonArray *artists = json_object_has_member (track_data, "artists") ?
    json_object_get_array_member (track_data, "artists") : NULL;
  if (artists && json_array_get_length (artists) > 0) {
    GString *names = g_string_new (NULL);
    for (guint i = 0; i < json_array_get_length (artists) && i < 2; i++) {
      JsonObject *artist = json_array_get_object_element (artists, i);
      const gchar *n = json_object_get_string_member_with_default (artist, "name", "");
      if (*n) {
        if (names->len > 0) g_string_append (names, ", ");
        g_string_append (names, n);
      }
    }
    gtk_label_set_text (self->artist_label, names->str);
    g_string_free (names, TRUE);
  }

  /* Album */
  JsonObject *album = json_object_has_member (track_data, "album") ?
    json_object_get_object_member (track_data, "album") : NULL;
  if (album) {
    const gchar *album_name = json_object_get_string_member_with_default (album, "name", "");
    gtk_label_set_text (self->album_label, album_name);
  }

  /* Duration */
  gint total_secs = (gint) (duration_ms / 1000);
  gchar *dur = g_strdup_printf ("%d:%02d", total_secs / 60, total_secs % 60);
  gtk_label_set_text (self->duration_label, dur);
  g_free (dur);
}

/*
 * The row's ordinal. Split out from set_native_track() because in a numbered
 * list the number is a property of the row's position, not of the track: a
 * removal renumbers everything below it without any of those tracks changing.
 */
void
spotifygtk_track_row_set_number (SpotifyGtkTrackRow *self, gint track_number)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));

  if (track_number > 0) {
    g_autofree gchar *num_text = g_strdup_printf ("%d", track_number);
    gtk_label_set_text (self->track_num, num_text);
    gtk_widget_set_visible (GTK_WIDGET (self->track_num), TRUE);
    gtk_widget_set_visible (GTK_WIDGET (self->album_art), FALSE);
  } else {
    /* No number: hide the column entirely rather than leaving a blank 24px
     * indent, which made unnumbered lists sit further right than numbered
     * ones. */
    gtk_widget_set_visible (GTK_WIDGET (self->track_num), FALSE);
  }
}

void
spotifygtk_track_row_set_native_track (SpotifyGtkTrackRow       *self,
                                       const SpotifyNativeTrack *track,
                                       gint                      track_number)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));
  if (!track) return;

  g_free (self->track_uri);
  g_free (self->track_id);
  self->track_uri = g_strdup (track->uri);

  /* The bare id is the last ':'-separated segment of spotify:track:<id>. */
  const gchar *last_colon = track->uri ? strrchr (track->uri, ':') : NULL;
  self->track_id = last_colon ? g_strdup (last_colon + 1) : NULL;

  gtk_label_set_text (self->title_label, track->name ? track->name : "Unknown track");
  gtk_label_set_text (self->artist_label, track->artists ? track->artists : "");
  gtk_label_set_text (self->album_label, track->album ? track->album : "");

  spotifygtk_track_row_set_number (self, track_number);

  gint total_secs = (gint) (track->duration_ms / 1000);
  g_autofree gchar *dur = g_strdup_printf ("%d:%02d", total_secs / 60, total_secs % 60);
  gtk_label_set_text (self->duration_label, dur);

  /*
   * The small variant, not the album's full-size art. A row draws 96px; the
   * widest variant averaged 112.5 KB, which is a 640px original downloaded and
   * thrown away once per row. Falls back to the large one when the album offers
   * only a single size.
   */
  row_request_cover (self, track->cover_id_small ? track->cover_id_small
                                                 : track->cover_id);
}

/*
 * Drop this row's artwork, leaving it in a state that can ask for it again.
 *
 * The pending id is kept deliberately, and so is the rest of the bookkeeping:
 * retry_cover() refuses to act while cover_shown is set, so clearing the image
 * without clearing that flag released the art and then declined to reload it,
 * for good. A returning page never rebinds its rows -- a GtkStack keeps
 * non-visible children realised -- so a rebind cannot be relied on to repair
 * it either.
 *
 * A fresh cancellable goes with it, so the reload is still cancellable.
 */
void
spotifygtk_track_row_release_cover (SpotifyGtkTrackRow *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));

  if (self->cover_cancellable) {
    g_cancellable_cancel (self->cover_cancellable);
    g_clear_object (&self->cover_cancellable);
  }
  if (!self->pending_cover_id)
    return;

  gtk_image_clear (self->album_art);
  gtk_widget_set_visible (GTK_WIDGET (self->album_art), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->track_num), self->row_number > 0);
  self->cover_shown = FALSE;
  self->cover_cancellable = g_cancellable_new ();
}

void
spotifygtk_track_row_set_show_album (SpotifyGtkTrackRow *self, gboolean show)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));
  self->show_album = show;
  gtk_widget_set_visible (GTK_WIDGET (self->album_label), show);
}

void
spotifygtk_track_row_set_show_artists (SpotifyGtkTrackRow *self, gboolean show)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));
  self->show_artists = show;
  gtk_widget_set_visible (GTK_WIDGET (self->artist_label), show);
}

void
spotifygtk_track_row_set_playing (SpotifyGtkTrackRow *self, gboolean is_playing, gboolean is_paused)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self));
  self->is_playing = is_playing;
  self->is_paused = is_paused;

  if (is_playing) {
    gtk_widget_add_css_class (GTK_WIDGET (self->title_label), "accent");
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self->title_label), "accent");
  }

  /*
   * The equaliser replaces the duration rather than joining it: the seek bar
   * already shows elapsed and total for whatever is playing, so repeating the
   * length on the row it applies to is the one place it says nothing.
   *
   * Shown whenever this is the current track, animating only while it is
   * actually advancing, so a paused row shows frozen bars.
   */
  gtk_widget_set_visible (self->eq_area, is_playing);
  gtk_widget_set_visible (GTK_WIDGET (self->duration_label), !is_playing);
  eq_set_running (self, is_playing && !is_paused);
}

const gchar *
spotifygtk_track_row_get_uri (SpotifyGtkTrackRow *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self), NULL);
  return self->track_uri;
}

const gchar *
spotifygtk_track_row_get_id (SpotifyGtkTrackRow *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_TRACK_ROW (self), NULL);
  return self->track_id;
}

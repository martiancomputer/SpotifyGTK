/*
 * playback_bar.c — Bottom playback bar implementation.
 *
 * Layout, three columns with the centre stacked:
 *
 *   [art] Title            ⏮  ▶  ⏭              🔊 ────────
 *         Artist      0:42 ──────────── 3:33
 *
 * The progress row sits under the transport buttons rather than beside
 * them, which is where media players normally put it and what the previous
 * single-row layout got wrong. The two side columns get equal width
 * requests so the centre group stays optically centred instead of drifting
 * with the length of the track title.
 */

#include "playback_bar.h"

/* Side columns are fixed and equal so the transport controls sit in the
 * true centre of the window regardless of how long the track name is. */
#define SIDE_COLUMN_WIDTH 260
#define ART_SIZE           56

struct _SpotifyGtkPlaybackBar {
  GtkBox parent_instance;

  /* Left: album art + track info */
  GtkImage *album_art;
  GtkLabel *track_label;
  GtkLabel *artist_label;

  /* Centre: transport + progress */
  GtkButton *prev_btn;
  GtkButton *play_btn;
  GtkButton *next_btn;
  GtkScale  *progress_scale;
  GtkLabel  *elapsed_label;
  GtkLabel  *duration_label;

  /* Right: volume + queue */
  GtkScale  *volume_scale;
  GtkButton *queue_btn;

  /* Toggles */
  GtkButton *shuffle_btn;
  GtkButton *repeat_btn;
  GtkButton *like_btn;
  gboolean   shuffle_on;
  gboolean   repeat_on;
  gboolean   liked;

  gboolean is_playing;
  gint64   position_ms;
  gint64   duration_ms;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkPlaybackBar, spotifygtk_playback_bar, GTK_TYPE_BOX)

enum {
  PLAY_CLICKED,
  PAUSE_CLICKED,
  NEXT_CLICKED,
  PREV_CLICKED,
  SEEK,
  VOLUME_CHANGED,
  LIKE_TOGGLED,
  SHUFFLE_TOGGLED,
  REPEAT_TOGGLED,
  QUEUE_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static gchar *
format_time (gint64 ms)
{
  if (ms <= 0)
    return g_strdup ("0:00");

  gint total = (gint) (ms / 1000);
  return g_strdup_printf ("%d:%02d", total / 60, total % 60);
}

/* === Callbacks === */

static void
on_play_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  g_signal_emit (self, signals[self->is_playing ? PAUSE_CLICKED : PLAY_CLICKED], 0);
  (void) button;
}

static void
on_prev_clicked (GtkButton *button, gpointer user_data)
{
  g_signal_emit (user_data, signals[PREV_CLICKED], 0);
  (void) button;
}

static void
on_next_clicked (GtkButton *button, gpointer user_data)
{
  g_signal_emit (user_data, signals[NEXT_CLICKED], 0);
  (void) button;
}

/* Toggles carry their state in a CSS class so the accent colour tracks it
 * without the button changing shape. */
static void
set_toggle_state (GtkButton *button, gboolean active)
{
  if (active)
    gtk_widget_add_css_class (GTK_WIDGET (button), "toggle-active");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (button), "toggle-active");
}

static void
on_shuffle_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  self->shuffle_on = !self->shuffle_on;
  set_toggle_state (self->shuffle_btn, self->shuffle_on);
  g_signal_emit (self, signals[SHUFFLE_TOGGLED], 0, self->shuffle_on);
  (void) button;
}

static void
on_repeat_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  self->repeat_on = !self->repeat_on;
  set_toggle_state (self->repeat_btn, self->repeat_on);
  g_signal_emit (self, signals[REPEAT_TOGGLED], 0, self->repeat_on);
  (void) button;
}

static void
on_queue_clicked (GtkButton *button, gpointer user_data)
{
  g_signal_emit (user_data, signals[QUEUE_CLICKED], 0);
  (void) button;
}

static void
on_like_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  self->liked = !self->liked;
  spotifygtk_playback_bar_set_liked (self, self->liked);
  g_signal_emit (self, signals[LIKE_TOGGLED], 0, self->liked);
  (void) button;
}

static void
on_volume_changed (GtkRange *range, gpointer user_data)
{
  g_signal_emit (user_data, signals[VOLUME_CHANGED], 0, (gint) gtk_range_get_value (range));
}

static void
on_seek_changed (GtkRange *range, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;

  if (self->duration_ms > 0) {
    gint64 new_pos = (gint64) (gtk_range_get_value (range) * (gdouble) self->duration_ms);
    g_signal_emit (self, signals[SEEK], 0, new_pos);
  }
}

/* === Construction === */

static void
spotifygtk_playback_bar_class_init (SpotifyGtkPlaybackBarClass *klass)
{
  signals[PLAY_CLICKED] = g_signal_new ("play-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[PAUSE_CLICKED] = g_signal_new ("pause-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[NEXT_CLICKED] = g_signal_new ("next-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[PREV_CLICKED] = g_signal_new ("prev-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[VOLUME_CHANGED] = g_signal_new ("volume-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SEEK] = g_signal_new ("seek",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT64);
  signals[QUEUE_CLICKED] = g_signal_new ("queue-clicked",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[LIKE_TOGGLED] = g_signal_new ("like-toggled",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[SHUFFLE_TOGGLED] = g_signal_new ("shuffle-toggled",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[REPEAT_TOGGLED] = g_signal_new ("repeat-toggled",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static GtkWidget *
build_left_column (SpotifyGtkPlaybackBar *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_size_request (box, SIDE_COLUMN_WIDTH, -1);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (box, GTK_ALIGN_START);
  gtk_widget_set_hexpand (box, FALSE);

  /* A fixed square. The art previously had no size constraint and stretched
   * to whatever height the row happened to be, which is the misshapen
   * thumbnail in the screenshot. */
  self->album_art = GTK_IMAGE (gtk_image_new_from_icon_name ("audio-x-generic-symbolic"));
  gtk_image_set_pixel_size (self->album_art, ART_SIZE / 2);
  gtk_widget_set_size_request (GTK_WIDGET (self->album_art), ART_SIZE, ART_SIZE);
  gtk_widget_set_halign (GTK_WIDGET (self->album_art), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self->album_art), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (self->album_art), "art-thumb");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->album_art));

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign (info, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (info, FALSE);
  /* Bounded width, with the labels ellipsizing inside it. Letting this
   * expand is what pushed the like button to the middle of the window and
   * dragged the transport group off centre. */
  gtk_widget_set_size_request (info, SIDE_COLUMN_WIDTH - ART_SIZE - 60, -1);

  self->track_label = GTK_LABEL (gtk_label_new ("Nothing playing"));
  gtk_label_set_xalign (self->track_label, 0.0);
  gtk_label_set_ellipsize (self->track_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (self->track_label), "bar-title");
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->track_label));

  self->artist_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (self->artist_label, 0.0);
  gtk_label_set_ellipsize (self->artist_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "bar-subtitle");
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->artist_label));

  gtk_box_append (GTK_BOX (box), info);

  self->like_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("emblem-favorite-symbolic"));
  gtk_widget_add_css_class (GTK_WIDGET (self->like_btn), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (self->like_btn), "circular");
  gtk_widget_add_css_class (GTK_WIDGET (self->like_btn), "transport-button");
  gtk_widget_set_valign (GTK_WIDGET (self->like_btn), GTK_ALIGN_CENTER);
  g_signal_connect (self->like_btn, "clicked", G_CALLBACK (on_like_clicked), self);
  /* Saving to the library needs a write endpoint the native stack does not
   * have yet, so this would only ever change its own colour. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->like_btn), FALSE);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->like_btn),
                               "Saving to your library isn\u2019t implemented yet");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->like_btn));

  return box;
}

static GtkWidget *
make_transport_button (const gchar *icon, const gchar *tooltip,
                       GCallback handler, gpointer self)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon);
  gtk_widget_add_css_class (button, "flat");
  gtk_widget_add_css_class (button, "circular");
  gtk_widget_add_css_class (button, "transport-button");
  gtk_widget_set_tooltip_text (button, tooltip);
  g_signal_connect (button, "clicked", handler, self);
  return button;
}

static GtkWidget *
build_centre_column (SpotifyGtkPlaybackBar *self)
{
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand (column, FALSE);
  gtk_widget_set_halign (column, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (column, GTK_ALIGN_CENTER);

  /* --- Transport row --- */
  GtkWidget *transport = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign (transport, GTK_ALIGN_CENTER);

  self->shuffle_btn = GTK_BUTTON (make_transport_button (
    "media-playlist-shuffle-symbolic", "Shuffle",
    G_CALLBACK (on_shuffle_clicked), self));
  gtk_box_append (GTK_BOX (transport), GTK_WIDGET (self->shuffle_btn));

  self->prev_btn = GTK_BUTTON (make_transport_button (
    "media-skip-backward-symbolic", "Previous",
    G_CALLBACK (on_prev_clicked), self));
  gtk_box_append (GTK_BOX (transport), GTK_WIDGET (self->prev_btn));

  /* Explicit square request: "circular" only rounds the corners, so without
   * equal width and height the play button rendered as an oval. */
  self->play_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("media-playback-start-symbolic"));
  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "circular");
  gtk_widget_add_css_class (GTK_WIDGET (self->play_btn), "play-button");
  gtk_widget_set_size_request (GTK_WIDGET (self->play_btn), 40, 40);
  gtk_widget_set_halign (GTK_WIDGET (self->play_btn), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self->play_btn), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->play_btn), "Play");
  g_signal_connect (self->play_btn, "clicked", G_CALLBACK (on_play_clicked), self);
  gtk_box_append (GTK_BOX (transport), GTK_WIDGET (self->play_btn));

  self->next_btn = GTK_BUTTON (make_transport_button (
    "media-skip-forward-symbolic", "Next",
    G_CALLBACK (on_next_clicked), self));
  gtk_box_append (GTK_BOX (transport), GTK_WIDGET (self->next_btn));

  self->repeat_btn = GTK_BUTTON (make_transport_button (
    "media-playlist-repeat-symbolic", "Repeat",
    G_CALLBACK (on_repeat_clicked), self));
  gtk_box_append (GTK_BOX (transport), GTK_WIDGET (self->repeat_btn));

  /* Skip needs a queue, which does not exist yet — the window's handlers
   * are empty. A control that does nothing when clicked reads as a bug. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->prev_btn), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->next_btn), FALSE);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->prev_btn), "Queue isn’t implemented yet");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->next_btn), "Queue isn’t implemented yet");

  gtk_box_append (GTK_BOX (column), transport);

  /* --- Progress row --- */
  GtkWidget *progress_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign (progress_row, GTK_ALIGN_CENTER);

  self->elapsed_label = GTK_LABEL (gtk_label_new ("0:00"));
  gtk_widget_add_css_class (GTK_WIDGET (self->elapsed_label), "time-label");
  gtk_label_set_width_chars (self->elapsed_label, 5);
  gtk_label_set_xalign (self->elapsed_label, 1.0);
  gtk_box_append (GTK_BOX (progress_row), GTK_WIDGET (self->elapsed_label));

  self->progress_scale = GTK_SCALE (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                                              0.0, 1.0, 0.001));
  gtk_scale_set_draw_value (self->progress_scale, FALSE);
  gtk_widget_set_size_request (GTK_WIDGET (self->progress_scale), 420, -1);
  gtk_widget_set_valign (GTK_WIDGET (self->progress_scale), GTK_ALIGN_CENTER);
  g_signal_connect (self->progress_scale, "value-changed", G_CALLBACK (on_seek_changed), self);

  /* "seek" still has no receiver: neither player_service nor native_engine
   * exposes a seek entry point. The bar renders position, but dragging is
   * disabled rather than silently snapping back. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->progress_scale), FALSE);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->progress_scale),
                               "Seeking isn’t wired to the playback engine yet");
  gtk_box_append (GTK_BOX (progress_row), GTK_WIDGET (self->progress_scale));

  self->duration_label = GTK_LABEL (gtk_label_new ("0:00"));
  gtk_widget_add_css_class (GTK_WIDGET (self->duration_label), "time-label");
  gtk_label_set_width_chars (self->duration_label, 5);
  gtk_label_set_xalign (self->duration_label, 0.0);
  gtk_box_append (GTK_BOX (progress_row), GTK_WIDGET (self->duration_label));

  gtk_box_append (GTK_BOX (column), progress_row);
  return column;
}

static GtkWidget *
build_right_column (SpotifyGtkPlaybackBar *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_size_request (box, SIDE_COLUMN_WIDTH, -1);
  gtk_widget_set_halign (box, GTK_ALIGN_END);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (box, FALSE);

  GtkWidget *icon = gtk_image_new_from_icon_name ("audio-volume-high-symbolic");
  gtk_widget_add_css_class (icon, "dim-text");
  gtk_box_append (GTK_BOX (box), icon);

  self->volume_scale = GTK_SCALE (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                                            0.0, 100.0, 1.0));
  gtk_scale_set_draw_value (self->volume_scale, FALSE);
  gtk_range_set_value (GTK_RANGE (self->volume_scale), 100.0);
  gtk_widget_set_size_request (GTK_WIDGET (self->volume_scale), 120, -1);
  gtk_widget_set_valign (GTK_WIDGET (self->volume_scale), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->volume_scale), "Volume");
  g_signal_connect (self->volume_scale, "value-changed", G_CALLBACK (on_volume_changed), self);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->volume_scale));

  self->queue_btn = GTK_BUTTON (make_transport_button (
    "view-list-symbolic", "Show the queue",
    G_CALLBACK (on_queue_clicked), self));
  gtk_widget_set_margin_start (GTK_WIDGET (self->queue_btn), 6);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->queue_btn));

  return box;
}

static void
spotifygtk_playback_bar_init (SpotifyGtkPlaybackBar *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "playback-bar");

  /* Spacing comes from CSS padding on .playback-bar, not widget margins:
   * a margin sits outside the widget's background, so the window colour
   * showed through as a lighter strip along the top and bottom edges. */

  /* GtkCenterBox, not three boxes in a row: it centres the middle child
   * against the whole width regardless of how wide the side children are,
   * which is the only way the transport group stays put as the track title
   * and window width change. */
  GtkWidget *center_box = gtk_center_box_new ();
  gtk_widget_set_hexpand (center_box, TRUE);
  gtk_center_box_set_start_widget (GTK_CENTER_BOX (center_box), build_left_column (self));
  gtk_center_box_set_center_widget (GTK_CENTER_BOX (center_box), build_centre_column (self));
  gtk_center_box_set_end_widget (GTK_CENTER_BOX (center_box), build_right_column (self));
  gtk_box_append (GTK_BOX (self), center_box);
}

SpotifyGtkPlaybackBar *
spotifygtk_playback_bar_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_PLAYBACK_BAR, NULL);
}

/* === Public setters === */

void
spotifygtk_playback_bar_set_track (SpotifyGtkPlaybackBar *self,
                                   const gchar *track_name,
                                   const gchar *artist)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));

  gtk_label_set_text (self->track_label, track_name ? track_name : "Nothing playing");
  gtk_label_set_text (self->artist_label, artist ? artist : "");
}

void
spotifygtk_playback_bar_set_playing (SpotifyGtkPlaybackBar *self, gboolean is_playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));

  self->is_playing = is_playing;
  gtk_button_set_icon_name (self->play_btn,
                            is_playing ? "media-playback-pause-symbolic"
                                       : "media-playback-start-symbolic");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->play_btn), is_playing ? "Pause" : "Play");
}

void
spotifygtk_playback_bar_set_progress (SpotifyGtkPlaybackBar *self,
                                      gint64 position_ms,
                                      gint64 duration_ms)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));

  self->position_ms = position_ms;
  self->duration_ms = duration_ms;

  g_autofree gchar *elapsed = format_time (position_ms);
  g_autofree gchar *total   = format_time (duration_ms);
  gtk_label_set_text (self->elapsed_label, elapsed);
  gtk_label_set_text (self->duration_label, total);

  if (duration_ms > 0) {
    gdouble fraction = (gdouble) position_ms / (gdouble) duration_ms;

    /* Block the handler: this is a display update, and letting it run would
     * emit "seek" as though the user had dragged the slider. */
    g_signal_handlers_block_by_func (self->progress_scale, on_seek_changed, self);
    gtk_range_set_value (GTK_RANGE (self->progress_scale), CLAMP (fraction, 0.0, 1.0));
    g_signal_handlers_unblock_by_func (self->progress_scale, on_seek_changed, self);
  }
}

void
spotifygtk_playback_bar_set_queue_count (SpotifyGtkPlaybackBar *self, gint count)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));
  (void) count;   /* no queue indicator in this layout yet */
}

void
spotifygtk_playback_bar_set_volume (SpotifyGtkPlaybackBar *self, gint percent)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));

  g_signal_handlers_block_by_func (self->volume_scale, on_volume_changed, self);
  gtk_range_set_value (GTK_RANGE (self->volume_scale), CLAMP (percent, 0, 100));
  g_signal_handlers_unblock_by_func (self->volume_scale, on_volume_changed, self);
}

void
spotifygtk_playback_bar_set_liked (SpotifyGtkPlaybackBar *self, gboolean liked)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));

  self->liked = liked;
  gtk_button_set_icon_name (self->like_btn,
                            liked ? "emblem-favorite-symbolic"
                                  : "emblem-favorite-symbolic");
  if (liked)
    gtk_widget_add_css_class (GTK_WIDGET (self->like_btn), "like-active");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self->like_btn), "like-active");
}

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
#include "cover_loader.h"

/* Side columns are fixed and equal so the transport controls sit in the
 * true centre of the window regardless of how long the track name is. */
#define SIDE_COLUMN_WIDTH 260
#define ART_SIZE           56

struct _SpotifyGtkPlaybackBar {
  GtkBox parent_instance;

  /* Left: album art + track info */
  GtkImage   *album_art;   /* placeholder icon */
  GtkPicture *album_pic;   /* cover, scaled to fill */
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
  GtkWidget *shuffle_smart_badge;
  GtkButton *repeat_btn;
  GtkButton *like_btn;
  SpotifyGtkShuffleMode shuffle_mode;
  SpotifyGtkRepeatMode repeat_mode;
  gboolean   liked;

  gboolean is_playing;
  gint64   position_ms;
  gint64   duration_ms;

  /* Seek: a drag emits many value-changed signals, so the actual seek is
   * committed once the user pauses (debounced). While seeking — and for a
   * grace window after, so the engine can catch up — position reports are
   * ignored so they don't yank the slider back. */
  gboolean user_seeking;
  guint    seek_commit_id;
  guint    seek_release_id;
};

/* Natural width of the now-playing title and artist, in characters. */
#define BAR_TEXT_MAX_CHARS 40

G_DEFINE_FINAL_TYPE (SpotifyGtkPlaybackBar, spotifygtk_playback_bar, GTK_TYPE_BOX)

enum {
  PLAY_CLICKED,
  PAUSE_CLICKED,
  NEXT_CLICKED,
  PREV_CLICKED,
  SEEK,
  VOLUME_CHANGED,
  LIKE_TOGGLED,
  SHUFFLE_MODE_CHANGED,
  REPEAT_CHANGED,
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
apply_shuffle_visual (SpotifyGtkPlaybackBar *self)
{
  gboolean smart = self->shuffle_mode == SPOTIFYGTK_SHUFFLE_SMART;
  set_toggle_state (self->shuffle_btn,
                    self->shuffle_mode != SPOTIFYGTK_SHUFFLE_OFF);
  if (smart)
    gtk_widget_add_css_class (GTK_WIDGET (self->shuffle_btn), "smart-shuffle");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self->shuffle_btn), "smart-shuffle");
  gtk_widget_set_visible (self->shuffle_smart_badge, smart);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->shuffle_btn),
    smart ? "Smart Shuffle" :
    self->shuffle_mode == SPOTIFYGTK_SHUFFLE_NORMAL ? "Shuffle" :
                                                       "Enable shuffle");
}

static void
on_shuffle_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  self->shuffle_mode = (self->shuffle_mode + 1) % 3;
  apply_shuffle_visual (self);
  g_signal_emit (self, signals[SHUFFLE_MODE_CHANGED], 0,
                 (guint) self->shuffle_mode);
  (void) button;
}

static void
apply_repeat_visual (SpotifyGtkPlaybackBar *self)
{
  /* A distinct icon for "one", because an active highlight alone cannot say
   * which of the two active modes is in force. */
  gtk_button_set_icon_name (self->repeat_btn,
    self->repeat_mode == SPOTIFYGTK_REPEAT_ONE ? "media-playlist-repeat-song-symbolic"
                                               : "media-playlist-repeat-symbolic");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->repeat_btn),
    self->repeat_mode == SPOTIFYGTK_REPEAT_ONE ? "Repeat one" :
    self->repeat_mode == SPOTIFYGTK_REPEAT_ALL ? "Repeat all" : "Repeat");
  set_toggle_state (self->repeat_btn, self->repeat_mode != SPOTIFYGTK_REPEAT_OFF);
}

static void
on_repeat_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  (void) button;

  self->repeat_mode = (self->repeat_mode + 1) % 3;
  apply_repeat_visual (self);
  g_signal_emit (self, signals[REPEAT_CHANGED], 0, (guint) self->repeat_mode);
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

static gchar *format_time (gint64 ms);

/* Grace window after committing a seek: position reports are ignored for this
 * long so the slider does not snap back to the old position before the engine
 * has moved. */
#define SEEK_RELEASE_GRACE_MS 900
/* Debounce a drag into a single seek once the user pauses moving. */
#define SEEK_COMMIT_DELAY_MS  220

static gboolean
clear_user_seeking (gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  self->user_seeking = FALSE;
  self->seek_release_id = 0;
  return G_SOURCE_REMOVE;
}

static gboolean
commit_seek (gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  self->seek_commit_id = 0;

  if (self->duration_ms > 0) {
    gint64 pos = (gint64) (gtk_range_get_value (GTK_RANGE (self->progress_scale))
                           * (gdouble) self->duration_ms);
    g_signal_emit (self, signals[SEEK], 0, pos);
  }

  /* Keep ignoring position reports briefly so the engine can reach the new
   * spot before the slider is handed back to it. */
  g_clear_handle_id (&self->seek_release_id, g_source_remove);
  self->seek_release_id = g_timeout_add (SEEK_RELEASE_GRACE_MS, clear_user_seeking, self);
  return G_SOURCE_REMOVE;
}

/* Fires only for user-driven changes: set_progress blocks this handler around
 * its programmatic updates. */
static void
on_seek_changed (GtkRange *range, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;
  if (self->duration_ms <= 0)
    return;

  self->user_seeking = TRUE;

  /* Show where the drag is pointing immediately, even though the seek itself
   * is only committed once movement settles. */
  gint64 dragged = (gint64) (gtk_range_get_value (range) * (gdouble) self->duration_ms);
  g_autofree gchar *elapsed = format_time (dragged);
  gtk_label_set_text (self->elapsed_label, elapsed);

  g_clear_handle_id (&self->seek_commit_id, g_source_remove);
  self->seek_commit_id = g_timeout_add (SEEK_COMMIT_DELAY_MS, commit_seek, self);
}

/* === Construction === */

static void
spotifygtk_playback_bar_dispose (GObject *object)
{
  SpotifyGtkPlaybackBar *self = SPOTIFYGTK_PLAYBACK_BAR (object);
  g_clear_handle_id (&self->seek_commit_id, g_source_remove);
  g_clear_handle_id (&self->seek_release_id, g_source_remove);
  G_OBJECT_CLASS (spotifygtk_playback_bar_parent_class)->dispose (object);
}

static void
spotifygtk_playback_bar_class_init (SpotifyGtkPlaybackBarClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_playback_bar_dispose;

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
  signals[SHUFFLE_MODE_CHANGED] = g_signal_new ("shuffle-mode-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[REPEAT_CHANGED] = g_signal_new ("repeat-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_UINT);
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
  /* Placeholder icon beneath, cover picture over the top -- see the same
   * arrangement in now_playing_panel.c for why GtkPicture rather than
   * GtkImage: only Picture scales the texture to fill the thumbnail. */
  self->album_art = GTK_IMAGE (gtk_image_new_from_icon_name ("audio-x-generic-symbolic"));
  gtk_image_set_pixel_size (self->album_art, ART_SIZE / 2);
  gtk_widget_add_css_class (GTK_WIDGET (self->album_art), "art-thumb");

  self->album_pic = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (self->album_pic, GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink (self->album_pic, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->album_pic), "art-thumb");
  gtk_widget_set_visible (GTK_WIDGET (self->album_pic), FALSE);

  GtkWidget *art = gtk_overlay_new ();
  gtk_widget_set_size_request (art, ART_SIZE, ART_SIZE);
  gtk_widget_set_halign (art, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (art, GTK_ALIGN_CENTER);
  gtk_overlay_set_child (GTK_OVERLAY (art), GTK_WIDGET (self->album_art));
  gtk_overlay_add_overlay (GTK_OVERLAY (art), GTK_WIDGET (self->album_pic));
  gtk_box_append (GTK_BOX (box), art);

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign (info, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (info, FALSE);

  /*
   * Room to breathe, with ellipsizing as the fallback rather than the norm.
   *
   * 18 characters truncated almost every real title -- the bar sits in a
   * GtkCenterBox, whose start widget gets its natural width, and this cap is
   * what that natural width was. Raising it does not force the window wider:
   * an ellipsizing label's *minimum* width stays small, so the text still
   * compresses when the window is genuinely short of space, which is the only
   * time it should.
   */
  self->track_label = GTK_LABEL (gtk_label_new ("Nothing playing"));
  gtk_label_set_xalign (self->track_label, 0.0);
  gtk_label_set_ellipsize (self->track_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (self->track_label, BAR_TEXT_MAX_CHARS);
  gtk_widget_add_css_class (GTK_WIDGET (self->track_label), "bar-title");
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->track_label));

  self->artist_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (self->artist_label, 0.0);
  gtk_label_set_ellipsize (self->artist_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (self->artist_label, BAR_TEXT_MAX_CHARS);
  gtk_widget_add_css_class (GTK_WIDGET (self->artist_label), "bar-subtitle");
  gtk_box_append (GTK_BOX (info), GTK_WIDGET (self->artist_label));

  gtk_box_append (GTK_BOX (box), info);

  self->like_btn = GTK_BUTTON (gtk_button_new_from_icon_name ("spotifygtk-heart-outline-symbolic"));
  gtk_widget_add_css_class (GTK_WIDGET (self->like_btn), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (self->like_btn), "circular");
  gtk_widget_add_css_class (GTK_WIDGET (self->like_btn), "transport-button");
  gtk_widget_set_valign (GTK_WIDGET (self->like_btn), GTK_ALIGN_CENTER);
  g_signal_connect (self->like_btn, "clicked", G_CALLBACK (on_like_clicked), self);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->like_btn), "Add to Liked Songs");
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
  /* Sits the transport cluster a little lower in the bar. */
  gtk_widget_set_margin_top (column, 6);

  /* --- Transport row --- */
  GtkWidget *transport = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign (transport, GTK_ALIGN_CENTER);

  self->shuffle_btn = GTK_BUTTON (make_transport_button (
    "media-playlist-shuffle-symbolic", "Shuffle",
    G_CALLBACK (on_shuffle_clicked), self));
  /* The accent colour means "enabled" for both shuffle modes. A tiny sparkle
   * distinguishes Smart Shuffle without replacing the familiar shuffle icon. */
  GtkWidget *shuffle_overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (shuffle_overlay),
                         gtk_image_new_from_icon_name (
                           "media-playlist-shuffle-symbolic"));
  self->shuffle_smart_badge = gtk_label_new ("✦");
  gtk_widget_add_css_class (self->shuffle_smart_badge, "smart-shuffle-badge");
  gtk_widget_set_halign (self->shuffle_smart_badge, GTK_ALIGN_END);
  gtk_widget_set_valign (self->shuffle_smart_badge, GTK_ALIGN_START);
  gtk_widget_set_visible (self->shuffle_smart_badge, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (shuffle_overlay),
                           self->shuffle_smart_badge);
  gtk_button_set_child (self->shuffle_btn, shuffle_overlay);
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
  gtk_widget_set_size_request (GTK_WIDGET (self->play_btn), 34, 34);
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

  /* Skip is driven by the window's play-context and queue. Both start
   * insensitive — nothing is playing at launch — and the window enables each
   * as a previous/next track becomes available (see
   * spotifygtk_playback_bar_set_skip_sensitive). */
  gtk_widget_set_sensitive (GTK_WIDGET (self->prev_btn), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->next_btn), FALSE);

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

  /* Draggable: a drag debounces into one "seek" emission (see on_seek_changed),
   * which the window routes to the engine's page-seek entry point. */
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->progress_scale), "Seek");
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

/* Restore the modes from settings at startup, without emitting -- the window
 * is the one that read them. */
void
spotifygtk_playback_bar_set_modes (SpotifyGtkPlaybackBar *self,
                                   SpotifyGtkShuffleMode shuffle,
                                   SpotifyGtkRepeatMode repeat)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));

  self->shuffle_mode = MIN (shuffle, SPOTIFYGTK_SHUFFLE_SMART);
  self->repeat_mode = repeat;
  apply_shuffle_visual (self);
  apply_repeat_visual (self);
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

  /*
   * The full text on hover, since both labels are capped.
   *
   * The cap is not cosmetic: ellipsizing does not bound a label's natural
   * width, so without max_width_chars a long title widens the whole bar and
   * with it the window's minimum size. The text has to stay clipped, so the
   * way to read it is a tooltip.
   *
   * Only when it is actually clipped -- a tooltip repeating a label you can
   * already read in full is just noise under the pointer.
   */
  gint cap = gtk_label_get_max_width_chars (self->track_label);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->track_label),
    (track_name && g_utf8_strlen (track_name, -1) > cap) ? track_name : NULL);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->artist_label),
    (artist && g_utf8_strlen (artist, -1) > cap) ? artist : NULL);
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

  /* Duration is always adopted, but while the user is dragging (or in the
   * grace window just after committing a seek) the position report is ignored
   * so it can't yank the slider away from the user or back to the old spot. */
  self->duration_ms = duration_ms;
  if (self->user_seeking)
    return;

  self->position_ms = position_ms;

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
  /*
   * A real pair now. Adwaita ships no outline heart to go with
   * emblem-favorite-symbolic (only starred/non-starred, a different visual
   * language), so one is compiled in -- see src/ui/icons/. Carrying the state
   * in colour alone was weak here because the accent colour *is* green: a
   * filled grey heart and a filled green heart differ only in hue, while a
   * stroked outline reads as unset at a glance.
   */
  gtk_button_set_icon_name (self->like_btn,
                            liked ? "spotifygtk-heart-filled-symbolic"
                                  : "spotifygtk-heart-outline-symbolic");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->like_btn),
                               liked ? "Remove from Liked Songs"
                                     : "Add to Liked Songs");
  if (liked)
    gtk_widget_add_css_class (GTK_WIDGET (self->like_btn), "like-active");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self->like_btn), "like-active");
}

static void
on_cover_loaded_spotifygtk_playback_bar (GdkTexture *texture, gpointer user_data)
{
  SpotifyGtkPlaybackBar *self = user_data;

  if (texture) {
    gtk_picture_set_paintable (self->album_pic, GDK_PAINTABLE (texture));
    gtk_widget_set_visible (GTK_WIDGET (self->album_pic), TRUE);
  } else {
    gtk_picture_set_paintable (self->album_pic, NULL);
    gtk_widget_set_visible (GTK_WIDGET (self->album_pic), FALSE);
  }
}

void
spotifygtk_playback_bar_set_cover (SpotifyGtkPlaybackBar *self, const gchar *cover_id)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));

  /* No cancellable: there is exactly one of these widgets, and a late cover
   * can only ever belong to the track it was asked for or be superseded by
   * the next call, which overwrites it anyway. */
  spotifygtk_cover_load (cover_id, 96, NULL, on_cover_loaded_spotifygtk_playback_bar, self);
}

void
spotifygtk_playback_bar_set_skip_sensitive (SpotifyGtkPlaybackBar *self,
                                            gboolean can_prev,
                                            gboolean can_next)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYBACK_BAR (self));
  gtk_widget_set_sensitive (GTK_WIDGET (self->prev_btn), can_prev);
  gtk_widget_set_sensitive (GTK_WIDGET (self->next_btn), can_next);
}

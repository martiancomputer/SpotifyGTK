/* Personalised Home: bounded display-only shelves, no eager track hydration. */
#include "home_page.h"
#include "smooth_scroll.h"
#include "settings.h"
#include "cover_loader.h"
#include "dialog_host.h"
#include "../log_file.h"
#include <string.h>

typedef struct {
  GtkWidget *tile, *open, *play;
  GtkPicture *picture;
  GtkLabel *title, *subtitle;
  gchar *uri, *cover;
  guint generation;
  gboolean loading;
} HomeQuickCard;

typedef struct {
  GtkWidget *box;
  GtkLabel *title, *subtitle;
  GtkWidget *heading, *see_all;
  SpotifyGtkAlbumGrid *grid;
} HomeShelf;

struct _SpotifyGtkHomePage {
  GtkBox parent_instance;
  GtkLabel *greeting, *status;
  GtkWidget *status_box, *retry;
  GtkBox *content;
  GtkScrolledWindow *scroller; /* owned by the widget tree */
  GtkWidget *jump_box, *rotation_box, *rotation_first, *rotation_title, *rotation_subtitle;
  GtkWidget *recent_button;
  HomeQuickCard quick[6];
  HomeShelf *fresh;
  guint quick_settle;
  gboolean covers_active;
  GPtrArray *shelves, *grids; /* borrowed widgets, owned by the widget tree */
  SpotifyNativeSession *session; /* window-owned */
  gchar *account;
  GCancellable *load_cancel;
  GBytes *snapshot;
  guint generation;
  gboolean loading;
  gint64 last_attempt, last_success;
};
G_DEFINE_FINAL_TYPE (SpotifyGtkHomePage, spotifygtk_home_page, GTK_TYPE_BOX)
enum { LOADING_CHANGED, GRID_ADDED, CONTEXT_REQUESTED, DESTINATION_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];
static void apply_visibility (SpotifyGtkHomePage *self);
static void schedule_quick_covers (SpotifyGtkHomePage *self);

static void
set_quick_visible (HomeQuickCard *card, gboolean visible)
{
  gtk_widget_set_visible (card->tile, visible);
  GtkWidget *parent = gtk_widget_get_parent (card->tile);
  if (parent) gtk_widget_set_visible (parent, visible);
}

static void
release_quick (HomeQuickCard *card)
{
  card->generation++;
  card->loading = FALSE;
  if (card->picture) gtk_picture_set_paintable (card->picture, NULL);
}

static void
set_loading (SpotifyGtkHomePage *self, gboolean loading)
{
  if (self->loading == loading) return;
  self->loading = loading;
  g_signal_emit (self, signals[LOADING_CHANGED], 0, loading);
}

static void
spotifygtk_home_page_dispose (GObject *object)
{
  SpotifyGtkHomePage *self = SPOTIFYGTK_HOME_PAGE (object);
  self->generation++;
  if (self->quick_settle) { g_source_remove (self->quick_settle); self->quick_settle = 0; }
  for (guint i = 0; i < G_N_ELEMENTS (self->quick); i++) {
    release_quick (&self->quick[i]);
    self->quick[i].picture = NULL;
    g_clear_pointer (&self->quick[i].uri, g_free);
    g_clear_pointer (&self->quick[i].cover, g_free);
  }
  if (self->load_cancel) g_cancellable_cancel (self->load_cancel);
  g_clear_object (&self->load_cancel);
  self->session = NULL;
  g_clear_pointer (&self->account, g_free);
  g_clear_pointer (&self->snapshot, g_bytes_unref);
  g_clear_pointer (&self->shelves, g_ptr_array_unref);
  g_clear_pointer (&self->grids, g_ptr_array_unref);
  G_OBJECT_CLASS (spotifygtk_home_page_parent_class)->dispose (object);
}

static void
spotifygtk_home_page_class_init (SpotifyGtkHomePageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_home_page_dispose;
  signals[LOADING_CHANGED] = g_signal_new ("loading-changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[GRID_ADDED] = g_signal_new ("grid-added",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, SPOTIFYGTK_TYPE_ALBUM_GRID);
  signals[CONTEXT_REQUESTED] = g_signal_new ("context-requested",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
  signals[DESTINATION_REQUESTED] = g_signal_new ("destination-requested",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static GtkLabel *
home_label (const gchar *text, const gchar *css)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (text));
  gtk_label_set_xalign (label, 0);
  gtk_label_set_ellipsize (label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (label), css);
  return label;
}

static void
on_quick_open (GtkButton *button, gpointer data)
{
  SpotifyGtkHomePage *self = data;
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "quick-index"));
  HomeQuickCard *card = &self->quick[index];
  if (card->uri && spotifygtk_home_uri_supported (card->uri))
    g_signal_emit (self, signals[CONTEXT_REQUESTED], 0, card->uri,
                   gtk_label_get_text (card->title), GTK_WIDGET (button) == card->play);
}

typedef struct { GWeakRef page; guint index, generation; } QuickCoverLoad;

static void
on_quick_cover (GdkTexture *texture, gpointer data)
{
  QuickCoverLoad *load = data;
  g_autoptr(SpotifyGtkHomePage) self = g_weak_ref_get (&load->page);
  if (self && self->quick[load->index].generation == load->generation) {
    HomeQuickCard *card = &self->quick[load->index];
    card->loading = FALSE;
    if (self->covers_active && texture &&
        spotifygtk_settings_get_media_mode (spotifygtk_settings_get_default ()) == SPOTIFYGTK_MEDIA_FULL)
      gtk_picture_set_paintable (card->picture, GDK_PAINTABLE (texture));
  }
  g_weak_ref_clear (&load->page);
  g_free (load);
}

static gboolean
update_quick_covers (gpointer data)
{
  SpotifyGtkHomePage *self = data;
  self->quick_settle = 0;
  graphene_rect_t rect;
  gint height = gtk_widget_get_height (GTK_WIDGET (self->scroller));
  gboolean near = self->covers_active && gtk_widget_get_mapped (self->jump_box) &&
    gtk_widget_compute_bounds (self->jump_box, GTK_WIDGET (self->scroller), &rect) &&
    rect.origin.y + rect.size.height > -160 && rect.origin.y < height + 160 &&
    spotifygtk_settings_get_media_mode (spotifygtk_settings_get_default ()) == SPOTIFYGTK_MEDIA_FULL;
  for (guint i = 0; i < G_N_ELEMENTS (self->quick); i++) {
    HomeQuickCard *card = &self->quick[i];
    if (!near) { release_quick (card); continue; }
    if (!card->uri || !card->cover || !*card->cover || card->loading ||
        gtk_picture_get_paintable (card->picture)) continue;
    QuickCoverLoad *load = g_new0 (QuickCoverLoad, 1);
    g_weak_ref_init (&load->page, self);
    load->index = i; load->generation = card->generation;
    card->loading = TRUE;
    spotifygtk_cover_load (card->cover, 64 * MAX (1, gtk_widget_get_scale_factor (card->tile)),
                           NULL, on_quick_cover, load);
  }
  return G_SOURCE_REMOVE;
}

static void
schedule_quick_covers (SpotifyGtkHomePage *self)
{
  if (!self->quick_settle)
    self->quick_settle = g_timeout_add (120, update_quick_covers, self);
}

static void
on_home_scroll (GtkAdjustment *adjustment, gpointer data)
{
  schedule_quick_covers (data);
  (void) adjustment;
}

static void
on_dialog_card (GtkButton *button, gpointer data)
{
  SpotifyGtkHomePage *self = data;
  g_autofree gchar *uri = g_strdup (g_object_get_data (G_OBJECT (button), "home-uri"));
  g_autofree gchar *title = g_strdup (g_object_get_data (G_OBJECT (button), "home-title"));
  GtkWidget *dialog = gtk_widget_get_ancestor (GTK_WIDGET (button), ADW_TYPE_DIALOG);
  if (dialog) adw_dialog_close (ADW_DIALOG (dialog));
  if (spotifygtk_home_uri_supported (uri))
    g_signal_emit (self, signals[CONTEXT_REQUESTED], 0, uri, title, FALSE);
}

static void
on_see_all (GtkButton *button, gpointer data)
{
  SpotifyGtkHomePage *self = data;
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "section-index"));
  g_autoptr(SpotifyHomeFeed) feed = spotifygtk_home_feed_decode (self->snapshot);
  if (!feed || !index || index > feed->sections->len) return;
  SpotifyHomeSection *section = g_ptr_array_index (feed->sections, index - 1);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (section->title, section->subtitle));
  spotifygtk_dialog_prepare (ADW_DIALOG (dialog));
  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroller), 420);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroller), TRUE);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  for (guint i = 0; i < section->cards->len; i++) {
    SpotifyHomeCard *card = g_ptr_array_index (section->cards, i);
    GtkWidget *row = gtk_button_new ();
    gtk_widget_add_css_class (row, "home-dialog-row");
    GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_can_target (labels, FALSE);
    gtk_box_append (GTK_BOX (labels), GTK_WIDGET (home_label (card->title, "home-card-title")));
    gtk_box_append (GTK_BOX (labels), GTK_WIDGET (home_label (card->subtitle, "home-card-subtitle")));
    gtk_button_set_child (GTK_BUTTON (row), labels);
    g_object_set_data_full (G_OBJECT (row), "home-uri", g_strdup (card->uri), g_free);
    g_object_set_data_full (G_OBJECT (row), "home-title", g_strdup (card->title), g_free);
    g_signal_connect_object (row, "clicked", G_CALLBACK (on_dialog_card), self, 0);
    gtk_box_append (GTK_BOX (box), row);
  }
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box);
  adw_alert_dialog_set_extra_child (dialog, scroller);
  adw_alert_dialog_add_response (dialog, "close", "Close");
  adw_alert_dialog_set_close_response (dialog, "close");
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_rotation (GtkButton *button, gpointer data)
{
  SpotifyGtkHomePage *self = data;
  const gchar *uri = g_object_get_data (G_OBJECT (button), "home-uri");
  const gchar *destination = g_object_get_data (G_OBJECT (button), "destination");
  if (destination) g_signal_emit (self, signals[DESTINATION_REQUESTED], 0, destination);
  else if (spotifygtk_home_uri_supported (uri))
    g_signal_emit (self, signals[CONTEXT_REQUESTED], 0, uri, "On repeat", FALSE);
}

static void
on_home_settings (SpotifyGtkSettings *settings, gpointer data)
{
  apply_visibility (data);
  schedule_quick_covers (data);
  (void) settings;
}

static HomeShelf *
add_shelf (SpotifyGtkHomePage *self)
{
  HomeShelf *s = g_new0 (HomeShelf, 1);
  s->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  s->title = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (s->title), "section-heading");
  gtk_label_set_xalign (s->title, 0);
  gtk_label_set_ellipsize (s->title, PANGO_ELLIPSIZE_END);
  s->heading = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_hexpand (GTK_WIDGET (s->title), TRUE);
  gtk_box_append (GTK_BOX (s->heading), GTK_WIDGET (s->title));
  s->see_all = gtk_button_new_with_label ("See all  ›");
  gtk_widget_add_css_class (s->see_all, "home-text-action");
  g_signal_connect (s->see_all, "clicked", G_CALLBACK (on_see_all), self);
  gtk_box_append (GTK_BOX (s->heading), s->see_all);
  gtk_box_append (GTK_BOX (s->box), s->heading);
  s->subtitle = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (s->subtitle), "dim-text");
  gtk_label_set_xalign (s->subtitle, 0);
  gtk_label_set_ellipsize (s->subtitle, PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_top (GTK_WIDGET (s->subtitle), 4);
  gtk_widget_set_margin_bottom (GTK_WIDGET (s->subtitle), 12);
  gtk_box_append (GTK_BOX (s->box), GTK_WIDGET (s->subtitle));
  s->grid = spotifygtk_album_grid_new_shelf ();
  spotifygtk_album_grid_set_home_style (s->grid);
  spotifygtk_album_grid_set_outer_viewport (s->grid, self->scroller);
  gtk_box_append (GTK_BOX (s->box), GTK_WIDGET (s->grid));
  gtk_box_append (self->content, s->box);
  g_ptr_array_add (self->shelves, s);
  g_ptr_array_add (self->grids, s->grid);
  g_signal_emit (self, signals[GRID_ADDED], 0, s->grid);
  return s;
}

static gboolean
title_matches (const gchar *title, const gchar *needle)
{
  g_autofree gchar *folded = g_utf8_casefold (title ?: "", -1);
  return strstr (folded, needle) != NULL;
}

static void
apply_visibility (SpotifyGtkHomePage *self)
{
  gtk_widget_set_visible (self->jump_box, self->quick[0].uri != NULL);
  gtk_widget_set_visible (self->rotation_box, TRUE);
  for (guint i = 0; i < self->shelves->len; i++) {
    HomeShelf *s = g_ptr_array_index (self->shelves, i);
    gboolean populated = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (s->box), "populated"));
    gboolean visible = populated;
    gtk_widget_set_visible (s->box, visible);
    if (!visible) spotifygtk_album_grid_suspend_outer_view (s->grid);
    else if (self->covers_active) spotifygtk_album_grid_reload_covers (s->grid);
  }
}

void
spotifygtk_home_page_set_feed (SpotifyGtkHomePage *self, const SpotifyHomeFeed *feed)
{
  g_return_if_fail (SPOTIFYGTK_IS_HOME_PAGE (self));
  g_return_if_fail (feed != NULL);
  g_autoptr(GBytes) snapshot = spotifygtk_home_feed_encode (feed);
  if (self->snapshot && g_bytes_equal (self->snapshot, snapshot)) {
    gtk_widget_set_visible (self->status_box, FALSE);
    return;
  }
  g_clear_pointer (&self->snapshot, g_bytes_unref);
  self->snapshot = g_bytes_ref (snapshot);
  guint count = MIN (feed->sections->len, SPOTIFYGTK_HOME_MAX_SECTIONS);
  gint jump = -1, fresh = -1;
  const SpotifyHomeCard *repeat = NULL;
  for (guint i = 0; i < count; i++) {
    SpotifyHomeSection *section = g_ptr_array_index (feed->sections, i);
    if (jump < 0 && (title_matches (section->title, "continue listening") ||
                     title_matches (section->title, "recently played") ||
                     title_matches (section->title, "jump back") ||
                     title_matches (section->title, "good morning") ||
                     title_matches (section->title, "good afternoon") ||
                     title_matches (section->title, "good evening"))) jump = i;
    if (fresh < 0 && (title_matches (section->title, "new release") ||
                      title_matches (section->title, "fresh") ||
                      title_matches (section->title, "new music"))) fresh = i;
    for (guint j = 0; j < section->cards->len; j++) {
      SpotifyHomeCard *card = g_ptr_array_index (section->cards, j);
      if (!repeat && title_matches (card->title, "on repeat") &&
          g_str_has_prefix (card->uri, "spotify:playlist:")) repeat = card;
    }
  }
  if (jump < 0 && count) jump = 0;
  gboolean new_releases = fresh >= 0;
  if (fresh < 0)
    for (guint i = 0; i < count; i++) if ((gint) i != jump) { fresh = i; break; }
  if (fresh < 0 && count) fresh = 0;
  g_object_set_data_full (G_OBJECT (self->rotation_first), "home-uri",
                          repeat ? g_strdup (repeat->uri) : NULL, g_free);
  g_object_set_data (G_OBJECT (self->rotation_first), "destination", repeat ? NULL : "library");
  gtk_label_set_text (GTK_LABEL (self->rotation_title), repeat ? "On repeat" : "Your library");
  gtk_label_set_text (GTK_LABEL (self->rotation_subtitle), repeat
    ? "Tracks you keep coming back to" : "Your saved albums and playlists");
  SpotifyHomeSection *recent = jump >= 0 ? g_ptr_array_index (feed->sections, jump) : NULL;
  g_object_set_data (G_OBJECT (self->recent_button), "section-index", GUINT_TO_POINTER (jump + 1));
  g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  guint next = 0;
  for (guint i = 0; recent && i < recent->cards->len && next < 5; i++) {
    SpotifyHomeCard *card = g_ptr_array_index (recent->cards, i);
    if (!spotifygtk_home_uri_supported (card->uri) ||
        g_str_equal (card->uri, "spotify:collection:tracks") ||
        g_str_has_suffix (card->uri, ":collection") || !g_hash_table_add (seen, card->uri)) continue;
    HomeQuickCard *q = &self->quick[next++];
    release_quick (q); g_free (q->uri); g_free (q->cover);
    q->uri = g_strdup (card->uri); q->cover = g_strdup (card->cover);
    gtk_label_set_text (q->title, card->title);
    gtk_label_set_text (q->subtitle, card->subtitle);
    gtk_widget_set_tooltip_text (q->open, card->title);
    gtk_image_set_from_icon_name (g_object_get_data (G_OBJECT (q->tile), "placeholder"),
                                  "media-optical-symbolic");
    set_quick_visible (q, TRUE);
  }
  HomeQuickCard *liked = &self->quick[next++];
  release_quick (liked); g_free (liked->uri); g_free (liked->cover);
  liked->uri = g_strdup ("spotify:collection:tracks"); liked->cover = NULL;
  gtk_label_set_text (liked->title, "Liked Songs");
  gtk_label_set_text (liked->subtitle, "Your collection");
  gtk_widget_set_tooltip_text (liked->open, "Liked Songs");
  gtk_image_set_from_icon_name (g_object_get_data (G_OBJECT (liked->tile), "placeholder"),
                                "emblem-favorite-symbolic");
  set_quick_visible (liked, TRUE);
  for (guint i = next; i < G_N_ELEMENTS (self->quick); i++) {
    release_quick (&self->quick[i]);
    g_clear_pointer (&self->quick[i].uri, g_free); g_clear_pointer (&self->quick[i].cover, g_free);
    set_quick_visible (&self->quick[i], FALSE);
  }
  self->fresh = NULL;
  for (guint i = 0; i < MAX (count, self->shelves->len); i++) {
    HomeShelf *s = i < self->shelves->len ? g_ptr_array_index (self->shelves, i) : add_shelf (self);
    if (i >= count) {
      gtk_widget_set_visible (s->box, FALSE);
      g_object_set_data (G_OBJECT (s->box), "populated", NULL);
      spotifygtk_album_grid_set_cards (s->grid, NULL, 0);
      continue;
    }
    SpotifyHomeSection *section = g_ptr_array_index (feed->sections, i);
    gboolean primary = (gint) i == fresh;
    if (primary) self->fresh = s;
    gtk_label_set_text (s->title, primary ? new_releases ? "Fresh for you" : "For you" : section->title);
    const gchar *subtitle = primary && !*section->subtitle ? new_releases
      ? "New releases picked around your listening." : "Picked around your listening." : section->subtitle;
    gtk_label_set_text (s->subtitle, subtitle);
    gtk_widget_set_visible (GTK_WIDGET (s->subtitle), *subtitle != '\0');
    g_object_set_data (G_OBJECT (s->see_all), "section-index", GUINT_TO_POINTER (i + 1));
    guint n = MIN (section->cards->len, SPOTIFYGTK_HOME_MAX_CARDS);
    SpotifyGtkCardSpec specs[SPOTIFYGTK_HOME_MAX_CARDS] = {0};
    for (guint j = 0; j < n; j++) {
      SpotifyHomeCard *card = g_ptr_array_index (section->cards, j);
      specs[j] = (SpotifyGtkCardSpec) { .uri = card->uri, .title = card->title,
        .subtitle = card->subtitle, .cover_id = card->cover };
    }
    spotifygtk_album_grid_set_cards (s->grid, specs, n);
    /* The compact recent section replaces its full shelf unless it is the
     * only recommendation source; remaining cards are accessible via Recent activity. */
    g_object_set_data (G_OBJECT (s->box), "populated",
                       GUINT_TO_POINTER (n > 0 && ((gint) i != jump || primary)));
  }
  if (self->fresh) gtk_box_reorder_child_after (self->content, self->fresh->box, self->jump_box);
  gtk_box_reorder_child_after (self->content, self->rotation_box, self->fresh ? self->fresh->box : self->jump_box);
  apply_visibility (self);
  schedule_quick_covers (self);
  gtk_widget_set_visible (self->status_box, FALSE);
}

typedef struct { GWeakRef page; guint generation; } HomeLoad;
static void
on_home_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  HomeLoad *load = user_data;
  g_autoptr(SpotifyGtkHomePage) self = g_weak_ref_get (&load->page);
  guint generation = load->generation;
  g_weak_ref_clear (&load->page); g_free (load);
  g_autoptr(GError) error = NULL;
  g_autoptr(SpotifyHomeFeed) feed = spotifygtk_native_session_load_home_finish (
    SPOTIFYGTK_NATIVE_SESSION (source), result, &error);
  spotifygtk_runtime_schedule_heap_trim ();
  if (!self || generation != self->generation) return;
  g_clear_object (&self->load_cancel);
  set_loading (self, FALSE);
  if (feed) {
    spotifygtk_home_page_set_feed (self, feed);
    self->last_success = g_get_monotonic_time ();
  } else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    gtk_label_set_text (self->status, "Couldn't refresh Home. Please try again.");
    gtk_widget_set_visible (self->status_box, TRUE);
    gtk_widget_set_visible (self->retry, TRUE);
    g_message ("Home refresh failed: %s", error ? error->message : "no feed");
  }
}

static void
refresh_home (SpotifyGtkHomePage *self)
{
  gint64 now = g_get_monotonic_time ();
  if (!self->session || self->loading ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY ||
      now - self->last_attempt < 10 * G_USEC_PER_SEC ||
      (self->last_success && now - self->last_success < 300 * G_USEC_PER_SEC)) return;
  self->last_attempt = now;
  self->load_cancel = g_cancellable_new ();
  set_loading (self, TRUE);
  HomeLoad *load = g_new0 (HomeLoad, 1);
  g_weak_ref_init (&load->page, self); load->generation = self->generation;
  spotifygtk_native_session_load_home (self->session, self->load_cancel, on_home_loaded, load);
}

static void
on_retry (GtkButton *button, gpointer data)
{
  SpotifyGtkHomePage *self = data;
  self->last_attempt = self->last_success = 0;
  refresh_home (self);
  (void) button;
}

void
spotifygtk_home_page_set_session (SpotifyGtkHomePage *self, SpotifyNativeSession *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_HOME_PAGE (self));
  if (self->session == session) { refresh_home (self); return; }
  self->generation++;
  if (self->load_cancel) g_cancellable_cancel (self->load_cancel);
  g_clear_object (&self->load_cancel);
  set_loading (self, FALSE);
  self->session = session;
  self->last_attempt = self->last_success = 0;
  if (!session) {
    gtk_widget_set_visible (self->retry, FALSE);
    return; /* keep cached cards browsable; window guards signed-out playback */
  }
  g_autofree gchar *account = spotifygtk_native_session_dup_username (session);
  if (g_strcmp0 (account, self->account) != 0) {
    g_clear_pointer (&self->snapshot, g_bytes_unref);
    for (guint i = 0; i < G_N_ELEMENTS (self->quick); i++) {
      release_quick (&self->quick[i]);
      g_clear_pointer (&self->quick[i].uri, g_free);
      g_clear_pointer (&self->quick[i].cover, g_free);
      set_quick_visible (&self->quick[i], FALSE);
    }
    gtk_widget_set_visible (self->jump_box, FALSE);
    g_object_set_data_full (G_OBJECT (self->rotation_first), "home-uri", NULL, NULL);
    g_object_set_data (G_OBJECT (self->rotation_first), "destination", "library");
    gtk_label_set_text (GTK_LABEL (self->rotation_title), "Your library");
    gtk_label_set_text (GTK_LABEL (self->rotation_subtitle), "Your saved albums and playlists");
    for (guint i = 0; i < self->shelves->len; i++) {
      HomeShelf *s = g_ptr_array_index (self->shelves, i);
      gtk_widget_set_visible (s->box, FALSE);
      g_object_set_data (G_OBJECT (s->box), "populated", NULL);
      spotifygtk_album_grid_set_cards (s->grid, NULL, 0);
    }
    g_free (self->account); self->account = g_strdup (account);
  }
  g_autoptr(SpotifyHomeFeed) cached = spotifygtk_native_session_get_cached_home (session);
  if (cached) spotifygtk_home_page_set_feed (self, cached);
  else {
    gtk_label_set_text (self->status, "Loading your Home…");
    gtk_widget_set_visible (self->status_box, TRUE);
  }
  if (gtk_widget_get_mapped (GTK_WIDGET (self))) refresh_home (self);
}

void
spotifygtk_home_page_clear_cache (SpotifyGtkHomePage *self)
{
  self->generation++;
  if (self->load_cancel) g_cancellable_cancel (self->load_cancel);
  g_clear_object (&self->load_cancel);
  set_loading (self, FALSE);
  g_clear_pointer (&self->snapshot, g_bytes_unref);
  self->last_attempt = self->last_success = 0;
  for (guint i = 0; i < G_N_ELEMENTS (self->quick); i++) {
    release_quick (&self->quick[i]);
    g_clear_pointer (&self->quick[i].uri, g_free);
    g_clear_pointer (&self->quick[i].cover, g_free);
    set_quick_visible (&self->quick[i], FALSE);
  }
  gtk_widget_set_visible (self->jump_box, FALSE);
  g_object_set_data_full (G_OBJECT (self->rotation_first), "home-uri", NULL, NULL);
  g_object_set_data (G_OBJECT (self->rotation_first), "destination", "library");
  gtk_label_set_text (GTK_LABEL (self->rotation_title), "Your library");
  gtk_label_set_text (GTK_LABEL (self->rotation_subtitle), "Your saved albums and playlists");
  for (guint i = 0; i < self->shelves->len; i++) {
    HomeShelf *s = g_ptr_array_index (self->shelves, i);
    gtk_widget_set_visible (s->box, FALSE);
    g_object_set_data (G_OBJECT (s->box), "populated", NULL);
    spotifygtk_album_grid_set_cards (s->grid, NULL, 0);
  }
  gtk_label_set_text (self->status, "Sign in to see your personalised Home. Local files are available in Library.");
  gtk_widget_set_visible (self->status_box, TRUE);
  gtk_widget_set_visible (self->retry, FALSE);
}

GPtrArray *
spotifygtk_home_page_get_grids (SpotifyGtkHomePage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_HOME_PAGE (self), NULL);
  return self->grids;
}

void
spotifygtk_home_page_set_covers_loaded (SpotifyGtkHomePage *self, gboolean loaded)
{
  self->covers_active = loaded;
  if (loaded) schedule_quick_covers (self);
  else for (guint i = 0; i < G_N_ELEMENTS (self->quick); i++) release_quick (&self->quick[i]);
  for (guint i = 0; i < self->grids->len; i++) {
    SpotifyGtkAlbumGrid *grid = g_ptr_array_index (self->grids, i);
    if (loaded) spotifygtk_album_grid_reload_covers (grid);
    else spotifygtk_album_grid_suspend_outer_view (grid);
  }
  if (!loaded) spotifygtk_runtime_schedule_heap_trim ();
}

static void
on_map (GtkWidget *widget, gpointer data)
{
  refresh_home (SPOTIFYGTK_HOME_PAGE (widget));
  schedule_quick_covers (SPOTIFYGTK_HOME_PAGE (widget));
  (void) data;
}

static GtkWidget *
create_quick_grid (SpotifyGtkHomePage *self)
{
  GtkWidget *flow = gtk_flow_box_new ();
  gtk_widget_add_css_class (flow, "home-quick-grid");
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 1);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 3);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 14);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 10);
  for (guint i = 0; i < G_N_ELEMENTS (self->quick); i++) {
    HomeQuickCard *card = &self->quick[i];
    card->tile = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (card->tile, "home-quick-tile");
    gtk_widget_set_size_request (card->tile, 260, 70);
    card->open = gtk_button_new ();
    gtk_widget_add_css_class (card->open, "home-quick-open");
    gtk_widget_set_hexpand (card->open, TRUE);
    GtkWidget *body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_can_target (body, FALSE);
    GtkWidget *art = gtk_overlay_new ();
    gtk_widget_add_css_class (art, "home-quick-art");
    gtk_widget_set_overflow (art, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request (art, 64, 64);
    GtkWidget *placeholder = gtk_image_new_from_icon_name ("media-optical-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (placeholder), 30);
    gtk_overlay_set_child (GTK_OVERLAY (art), placeholder);
    g_object_set_data (G_OBJECT (card->tile), "placeholder", placeholder);
    card->picture = GTK_PICTURE (gtk_picture_new ());
    gtk_picture_set_content_fit (card->picture, GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink (card->picture, TRUE);
    gtk_overlay_add_overlay (GTK_OVERLAY (art), GTK_WIDGET (card->picture));
    gtk_box_append (GTK_BOX (body), art);
    GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign (labels, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand (labels, TRUE);
    card->title = home_label ("", "home-card-title");
    card->subtitle = home_label ("", "home-card-subtitle");
    gtk_label_set_max_width_chars (card->title, 20);
    gtk_label_set_max_width_chars (card->subtitle, 22);
    gtk_box_append (GTK_BOX (labels), GTK_WIDGET (card->title));
    gtk_box_append (GTK_BOX (labels), GTK_WIDGET (card->subtitle));
    gtk_box_append (GTK_BOX (body), labels);
    gtk_button_set_child (GTK_BUTTON (card->open), body);
    gtk_box_append (GTK_BOX (card->tile), card->open);
    card->play = gtk_button_new_from_icon_name ("media-playback-start-symbolic");
    gtk_widget_add_css_class (card->play, "home-quick-play");
    gtk_widget_set_tooltip_text (card->play, "Play");
    gtk_widget_set_valign (card->play, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (card->tile), card->play);
    g_object_set_data (G_OBJECT (card->open), "quick-index", GUINT_TO_POINTER (i));
    g_object_set_data (G_OBJECT (card->play), "quick-index", GUINT_TO_POINTER (i));
    g_signal_connect (card->open, "clicked", G_CALLBACK (on_quick_open), self);
    g_signal_connect (card->play, "clicked", G_CALLBACK (on_quick_open), self);
    gtk_flow_box_insert (GTK_FLOW_BOX (flow), card->tile, -1);
    set_quick_visible (card, FALSE);
  }
  return flow;
}

static GtkWidget *
create_rotation_tile (SpotifyGtkHomePage *self, const gchar *title, const gchar *subtitle,
                      const gchar *icon, const gchar *css, const gchar *destination)
{
  GtkWidget *button = gtk_button_new ();
  gtk_widget_add_css_class (button, "home-rotation-tile");
  gtk_widget_set_size_request (button, 260, 104);
  g_object_set_data (G_OBJECT (button), "destination", (gpointer) destination);
  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_widget_set_can_target (body, FALSE);
  GtkWidget *glyph = gtk_image_new_from_icon_name (icon);
  gtk_image_set_pixel_size (GTK_IMAGE (glyph), 32);
  gtk_widget_set_size_request (glyph, 76, 76);
  gtk_widget_add_css_class (glyph, "home-rotation-icon");
  gtk_widget_add_css_class (glyph, css);
  gtk_box_append (GTK_BOX (body), glyph);
  GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_valign (labels, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (labels, TRUE);
  GtkWidget *name = GTK_WIDGET (home_label (title, "home-card-title"));
  GtkWidget *detail = GTK_WIDGET (home_label (subtitle, "home-card-subtitle"));
  gtk_label_set_max_width_chars (GTK_LABEL (name), 18);
  gtk_label_set_max_width_chars (GTK_LABEL (detail), 20);
  gtk_box_append (GTK_BOX (labels), name);
  gtk_box_append (GTK_BOX (labels), detail);
  gtk_box_append (GTK_BOX (body), labels);
  GtkWidget *arrow = gtk_image_new_from_icon_name ("go-next-symbolic");
  gtk_widget_add_css_class (arrow, "dim-text");
  gtk_widget_set_valign (arrow, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (body), arrow);
  gtk_button_set_child (GTK_BUTTON (button), body);
  g_signal_connect (button, "clicked", G_CALLBACK (on_rotation), self);
  if (!self->rotation_first) {
    self->rotation_first = button; self->rotation_title = name; self->rotation_subtitle = detail;
  }
  return button;
}

static void
spotifygtk_home_page_init (SpotifyGtkHomePage *self)
{
  self->shelves = g_ptr_array_new_with_free_func (g_free);
  self->grids = g_ptr_array_new ();
  self->covers_active = TRUE;
  gtk_widget_add_css_class (GTK_WIDGET (self), "home-dashboard");
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  self->scroller = GTK_SCROLLED_WINDOW (scroller);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);
  spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (scroller), GTK_ORIENTATION_VERTICAL);
  self->content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 30));
  gtk_widget_set_margin_start (GTK_WIDGET (self->content), 34);
  gtk_widget_set_margin_end (GTK_WIDGET (self->content), 34);
  gtk_widget_set_margin_top (GTK_WIDGET (self->content), 22);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->content), 24);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  gint hour = g_date_time_get_hour (now);
  self->greeting = GTK_LABEL (gtk_label_new (hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening"));
  gtk_widget_add_css_class (GTK_WIDGET (self->greeting), "title-text");
  gtk_label_set_xalign (self->greeting, 0);
  gtk_box_append (GTK_BOX (header), GTK_WIDGET (self->greeting));
  gtk_box_append (GTK_BOX (header), GTK_WIDGET (home_label ("Your music, right where you left it.", "dim-text")));
  gtk_box_append (self->content, header);
  self->status_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  self->status = GTK_LABEL (gtk_label_new ("Sign in to see your personalised Home. Local files are available in Library."));
  gtk_label_set_xalign (self->status, 0);
  gtk_label_set_wrap (self->status, TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->status), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->status), "dim-text");
  gtk_box_append (GTK_BOX (self->status_box), GTK_WIDGET (self->status));
  self->retry = gtk_button_new_with_label ("Retry");
  gtk_widget_add_css_class (self->retry, "flat");
  gtk_widget_set_visible (self->retry, FALSE);
  gtk_box_append (GTK_BOX (self->status_box), self->retry);
  g_signal_connect (self->retry, "clicked", G_CALLBACK (on_retry), self);
  gtk_box_append (self->content, self->status_box);
  self->jump_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  GtkWidget *jump_heading = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *jump_title = GTK_WIDGET (home_label ("Jump back in", "section-heading"));
  gtk_widget_set_hexpand (jump_title, TRUE);
  gtk_box_append (GTK_BOX (jump_heading), jump_title);
  self->recent_button = gtk_button_new_with_label ("RECENT ACTIVITY");
  gtk_widget_add_css_class (self->recent_button, "home-eyebrow");
  g_signal_connect (self->recent_button, "clicked", G_CALLBACK (on_see_all), self);
  gtk_box_append (GTK_BOX (jump_heading), self->recent_button);
  gtk_box_append (GTK_BOX (self->jump_box), jump_heading);
  gtk_box_append (GTK_BOX (self->jump_box), create_quick_grid (self));
  gtk_widget_set_visible (self->jump_box, FALSE);
  gtk_box_append (self->content, self->jump_box);
  self->rotation_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *rotation_heading = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *rotation_label = GTK_WIDGET (home_label ("Your rotation", "section-heading"));
  gtk_widget_set_hexpand (rotation_label, TRUE);
  gtk_box_append (GTK_BOX (rotation_heading), rotation_label);
  gtk_box_append (GTK_BOX (rotation_heading), GTK_WIDGET (home_label ("More from your library", "home-card-subtitle")));
  gtk_box_append (GTK_BOX (self->rotation_box), rotation_heading);
  GtkWidget *rotation = gtk_flow_box_new ();
  gtk_widget_add_css_class (rotation, "home-quick-grid");
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (rotation), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (rotation), TRUE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (rotation), 1);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (rotation), 3);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (rotation), 14);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (rotation), 10);
  gtk_flow_box_insert (GTK_FLOW_BOX (rotation), create_rotation_tile (self,
    "Your library", "Your saved albums and playlists", "media-playlist-repeat-symbolic", "home-repeat-icon", "library"), -1);
  gtk_flow_box_insert (GTK_FLOW_BOX (rotation), create_rotation_tile (self,
    "Local collection", "Your music, on this device", "folder-music-symbolic", "home-local-icon", "local"), -1);
  gtk_flow_box_insert (GTK_FLOW_BOX (rotation), create_rotation_tile (self,
    "Recently liked", "The latest additions to your library", "emblem-favorite-symbolic", "home-liked-icon", "liked"), -1);
  gtk_box_append (GTK_BOX (self->rotation_box), rotation);
  gtk_box_append (self->content, self->rotation_box);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (self->content));
  gtk_box_append (GTK_BOX (self), scroller);
  g_signal_connect_object (gtk_scrolled_window_get_vadjustment (self->scroller), "value-changed",
                            G_CALLBACK (on_home_scroll), self, 0);
  g_signal_connect_object (spotifygtk_settings_get_default (), "changed",
                            G_CALLBACK (on_home_settings), self, 0);
  g_signal_connect (self, "map", G_CALLBACK (on_map), NULL);
}

SpotifyGtkHomePage *
spotifygtk_home_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_HOME_PAGE, NULL);
}

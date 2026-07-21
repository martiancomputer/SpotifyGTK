/*
 * track_list.c — Reusable scrolling list of TrackRows.
 */

#include "track_list.h"
#include "track_row.h"

struct _SpotifyGtkTrackList {
  GtkBox parent_instance;

  GtkLabel   *status;
  GtkListBox *list;

  gboolean numbered;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkTrackList, spotifygtk_track_list, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
emit_for_row (SpotifyGtkTrackList *self, GtkWidget *row)
{
  JsonObject *track = g_object_get_data (G_OBJECT (row), "track-json");
  if (track)
    g_signal_emit (self, signals[TRACK_ACTIVATED], 0, track);
}

static void
on_row_play_clicked (SpotifyGtkTrackRow *row, gpointer user_data)
{
  emit_for_row (user_data, GTK_WIDGET (row));
}

static void
on_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  emit_for_row (user_data, GTK_WIDGET (row));
  (void) box;
}

static void
spotifygtk_track_list_class_init (SpotifyGtkTrackListClass *klass)
{
  signals[TRACK_ACTIVATED] = g_signal_new ("track-activated",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
spotifygtk_track_list_init (SpotifyGtkTrackList *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 8);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  self->status = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->status), "dim-text");
  gtk_label_set_xalign (self->status, 0.0);
  gtk_widget_set_visible (GTK_WIDGET (self->status), FALSE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->status));

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  /* Overlay scrolling floats the scrollbar on top of the rows, so it landed
   * over the duration column. Turning it off gives the scrollbar its own
   * gutter beside the list instead of on it. */
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);

  self->list = GTK_LIST_BOX (gtk_list_box_new ());
  /* Just enough to clear the scrollbar gutter; more than this leaves a
   * visible dead strip before the Now Playing panel. */
  gtk_widget_set_margin_end (GTK_WIDGET (self->list), 2);
  gtk_list_box_set_selection_mode (self->list, GTK_SELECTION_NONE);
  g_signal_connect (self->list, "row-activated", G_CALLBACK (on_row_activated), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (self->list));

  gtk_box_append (GTK_BOX (self), scroller);
}

SpotifyGtkTrackList *
spotifygtk_track_list_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_TRACK_LIST, NULL);
}

void
spotifygtk_track_list_clear (SpotifyGtkTrackList *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->list))))
    gtk_list_box_remove (self->list, child);
}

void
spotifygtk_track_list_set_status (SpotifyGtkTrackList *self, const gchar *message)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  if (message && *message) {
    gtk_label_set_text (self->status, message);
    gtk_widget_set_visible (GTK_WIDGET (self->status), TRUE);
  } else {
    gtk_widget_set_visible (GTK_WIDGET (self->status), FALSE);
  }
}

void
spotifygtk_track_list_set_numbered (SpotifyGtkTrackList *self, gboolean numbered)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));
  self->numbered = numbered;
}

void
spotifygtk_track_list_set_playing_uri (SpotifyGtkTrackList *self,
                                       const gchar *uri,
                                       gboolean     playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->list));
       child != NULL;
       child = gtk_widget_get_next_sibling (child)) {
    if (!SPOTIFYGTK_IS_TRACK_ROW (child))
      continue;

    SpotifyGtkTrackRow *row = SPOTIFYGTK_TRACK_ROW (child);
    const gchar *row_uri = spotifygtk_track_row_get_uri (row);
    gboolean is_current = uri && row_uri && g_strcmp0 (uri, row_uri) == 0;

    spotifygtk_track_row_set_playing (row, is_current, is_current && !playing);
  }
}

static SpotifyNativeTrack *
native_track_copy (const SpotifyNativeTrack *src)
{
  SpotifyNativeTrack *copy = g_new0 (SpotifyNativeTrack, 1);
  copy->uri         = g_strdup (src->uri);
  copy->name        = g_strdup (src->name);
  copy->artists     = g_strdup (src->artists);
  copy->album       = g_strdup (src->album);
  copy->duration_ms = src->duration_ms;
  copy->is_explicit = src->is_explicit;
  return copy;
}

void
spotifygtk_track_list_set_native_tracks (SpotifyGtkTrackList *self, GPtrArray *tracks)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  spotifygtk_track_list_clear (self);

  if (!tracks || tracks->len == 0) {
    spotifygtk_track_list_set_status (self, "Nothing here yet.");
    return;
  }

  for (guint i = 0; i < tracks->len; i++) {
    const SpotifyNativeTrack *track = g_ptr_array_index (tracks, i);
    if (!track || !track->uri)
      continue;

    SpotifyGtkTrackRow *row = spotifygtk_track_row_new ();
    spotifygtk_track_row_set_native_track (row, track,
                                           self->numbered ? (gint) (i + 1) : 0);

    /* The row outlives the caller's array, so it owns a copy. */
    g_object_set_data_full (G_OBJECT (row), "track-json",
                            native_track_copy (track),
                            (GDestroyNotify) spotifygtk_native_track_free);

    g_signal_connect (row, "play-clicked", G_CALLBACK (on_row_play_clicked), self);
    gtk_widget_add_css_class (GTK_WIDGET (row), "list-row");
    gtk_list_box_append (self->list, GTK_WIDGET (row));
  }

  spotifygtk_track_list_set_status (self, NULL);
}

void
spotifygtk_track_list_set_tracks (SpotifyGtkTrackList *self, JsonArray *items)
{
  g_return_if_fail (SPOTIFYGTK_IS_TRACK_LIST (self));

  spotifygtk_track_list_clear (self);

  guint n = items ? json_array_get_length (items) : 0;
  if (n == 0) {
    spotifygtk_track_list_set_status (self, "Nothing here yet.");
    return;
  }

  guint shown = 0;
  for (guint i = 0; i < n; i++) {
    JsonObject *entry = json_array_get_object_element (items, i);
    if (!entry)
      continue;

    /* /me/tracks and recently-played wrap the real track one level down. */
    JsonObject *track = entry;
    if (json_object_has_member (entry, "track")) {
      JsonNode *node = json_object_get_member (entry, "track");
      if (JSON_NODE_HOLDS_OBJECT (node))
        track = json_object_get_object_member (entry, "track");
    }

    /* Local files and unavailable entries come back without a URI. */
    if (!json_object_has_member (track, "uri"))
      continue;

    SpotifyGtkTrackRow *row = spotifygtk_track_row_new ();
    spotifygtk_track_row_set_track (row, track, self->numbered ? (gint) (shown + 1) : 0);
    g_object_set_data_full (G_OBJECT (row), "track-json",
                            json_object_ref (track),
                            (GDestroyNotify) json_object_unref);
    g_signal_connect (row, "play-clicked", G_CALLBACK (on_row_play_clicked), self);
    gtk_widget_add_css_class (GTK_WIDGET (row), "list-row");
    gtk_list_box_append (self->list, GTK_WIDGET (row));
    shown++;
  }

  spotifygtk_track_list_set_status (self, shown == 0 ? "Nothing playable here." : NULL);
}

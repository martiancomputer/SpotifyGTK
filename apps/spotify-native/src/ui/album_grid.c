/*
 * album_grid.c — A grid or shelf of album cards, derived from a track list.
 *
 * The native stack has one catalog primitive: resolve a context URI to a flat
 * list of tracks. There is no categorised search and no album-listing
 * endpoint. But every SpotifyNativeTrack already carries the album it belongs
 * to — its uri, name and cover id — so the set of *distinct albums* present in
 * any resolved track list is real data we already hold, not something invented
 * to fill the page. This widget derives that set and shows it as cards.
 *
 * Clicking a card emits "album-activated" with the album URI, which the window
 * hands to the same /context-resolve path that "Go to Album" already uses — so
 * a card opens the real album, not a mock.
 *
 * Two shapes from one widget: `wrap` TRUE lays the cards out in a wrapping
 * GtkFlowBox (the Library grid); FALSE puts them in a single horizontal row
 * inside a scroller (a Home/Search shelf).
 */

#include "album_grid.h"
#include "cover_loader.h"

#define CARD_ART_PX     150   /* on-screen card art size */
#define CARD_DECODE_PX  180   /* decode a touch larger so downscaling stays crisp */

struct _SpotifyGtkAlbumGrid {
  GtkBox parent_instance;

  GtkWidget *host;      /* GtkFlowBox (wrap) or GtkBox (shelf); holds the cards */
  gboolean   wrap;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkAlbumGrid, spotifygtk_album_grid, GTK_TYPE_BOX)

enum { ALBUM_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* A pending cover load is cancelled when its card goes away, so a late decode
 * never paints onto a card that has since been recycled by a fresh load. */
static void
cancel_and_unref (gpointer data)
{
  GCancellable *c = data;
  g_cancellable_cancel (c);
  g_object_unref (c);
}

static void
on_card_cover_loaded (GdkTexture *texture, gpointer user_data)
{
  GtkImage *art = user_data;
  if (texture)
    gtk_image_set_from_paintable (art, GDK_PAINTABLE (texture));
}

static void
on_card_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkAlbumGrid *self = user_data;
  const gchar *uri  = g_object_get_data (G_OBJECT (button), "album-uri");
  const gchar *name = g_object_get_data (G_OBJECT (button), "album-name");
  if (uri)
    g_signal_emit (self, signals[ALBUM_ACTIVATED], 0, uri, name);
}

/* One album card: cover over a title and artist line, the whole thing a flat
 * button so a click opens the album. */
static GtkWidget *
build_card (SpotifyGtkAlbumGrid *self,
            const gchar *uri, const gchar *name,
            const gchar *artist, const gchar *cover_id)
{
  GtkWidget *card = gtk_button_new ();
  gtk_widget_add_css_class (card, "media-card");
  gtk_widget_add_css_class (card, "flat");
  gtk_widget_set_size_request (card, CARD_ART_PX + 24, -1);
  gtk_button_set_has_frame (GTK_BUTTON (card), FALSE);

  g_object_set_data_full (G_OBJECT (card), "album-uri", g_strdup (uri), g_free);
  if (name)
    g_object_set_data_full (G_OBJECT (card), "album-name", g_strdup (name), g_free);
  g_signal_connect (card, "clicked", G_CALLBACK (on_card_clicked), self);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (box, 8);
  gtk_widget_set_margin_end (box, 8);
  gtk_widget_set_margin_top (box, 8);
  gtk_widget_set_margin_bottom (box, 8);

  GtkWidget *art = gtk_image_new_from_icon_name ("media-optical-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (art), CARD_ART_PX);
  gtk_widget_add_css_class (art, "art-large");
  gtk_box_append (GTK_BOX (box), art);

  if (cover_id && *cover_id) {
    GCancellable *cancel = g_cancellable_new ();
    g_object_set_data_full (G_OBJECT (art), "cover-cancel", cancel, cancel_and_unref);
    spotifygtk_cover_load (cover_id, CARD_DECODE_PX, cancel, on_card_cover_loaded, art);
  }

  GtkWidget *title = gtk_label_new (name ? name : "Unknown album");
  gtk_widget_add_css_class (title, "media-card-title");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (title), 14);
  gtk_box_append (GTK_BOX (box), title);

  if (artist && *artist) {
    GtkWidget *sub = gtk_label_new (artist);
    gtk_widget_add_css_class (sub, "media-card-subtitle");
    gtk_label_set_xalign (GTK_LABEL (sub), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (sub), 16);
    gtk_box_append (GTK_BOX (box), sub);
  }

  gtk_button_set_child (GTK_BUTTON (card), box);
  return card;
}

static void
append_card (SpotifyGtkAlbumGrid *self, GtkWidget *card)
{
  if (self->wrap)
    gtk_flow_box_append (GTK_FLOW_BOX (self->host), card);
  else
    gtk_box_append (GTK_BOX (self->host), card);
}

void
spotifygtk_album_grid_clear (SpotifyGtkAlbumGrid *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self));

  /* Removing the child triggers the cover-cancel destroy-notify, so any
   * in-flight decode for a card being dropped is cancelled here. */
  if (self->wrap) {
    gtk_flow_box_remove_all (GTK_FLOW_BOX (self->host));
  } else {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (self->host)))
      gtk_box_remove (GTK_BOX (self->host), child);
  }
}

guint
spotifygtk_album_grid_set_from_tracks (SpotifyGtkAlbumGrid *self,
                                       GPtrArray           *tracks,
                                       guint                max_albums)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self), 0);

  spotifygtk_album_grid_clear (self);

  if (!tracks || tracks->len == 0)
    return 0;

  /* Distinct albums in first-seen order. The keys are borrowed from the live
   * tracks array, which outlives this call, so the set need not copy them. */
  g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  guint shown = 0;

  for (guint i = 0; i < tracks->len; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (tracks, i);
    if (!t || !t->album_uri || !*t->album_uri)
      continue;
    if (g_hash_table_contains (seen, t->album_uri))
      continue;
    g_hash_table_add (seen, t->album_uri);

    append_card (self, build_card (self, t->album_uri, t->album,
                                   t->artists, t->cover_id));

    if (++shown >= max_albums)
      break;
  }

  return shown;
}

static void
spotifygtk_album_grid_class_init (SpotifyGtkAlbumGridClass *klass)
{
  signals[ALBUM_ACTIVATED] = g_signal_new (
    "album-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
    NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

static void
spotifygtk_album_grid_init (SpotifyGtkAlbumGrid *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
}

static SpotifyGtkAlbumGrid *
album_grid_new (gboolean wrap)
{
  SpotifyGtkAlbumGrid *self = g_object_new (SPOTIFYGTK_TYPE_ALBUM_GRID, NULL);
  self->wrap = wrap;

  if (wrap) {
    GtkWidget *flow = gtk_flow_box_new ();
    gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
    gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 8);
    gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 8);
    gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 2);
    gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 8);
    gtk_widget_set_hexpand (flow, TRUE);
    self->host = flow;
    gtk_box_append (GTK_BOX (self), flow);
  } else {
    GtkWidget *scroller = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    /* Size the scroller to exactly one card row, and let the (thin, auto-hiding)
     * horizontal bar float over the empty strip the row's bottom margin leaves
     * — so it never sits on top of the artist labels the way a bar drawn at the
     * content's own bottom edge did. */
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroller), TRUE);
    gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), TRUE);
    gtk_widget_set_hexpand (scroller, TRUE);

    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_bottom (row, 14);
    self->host = row;
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), row);
    gtk_box_append (GTK_BOX (self), scroller);
  }

  return self;
}

SpotifyGtkAlbumGrid *
spotifygtk_album_grid_new_shelf (void)
{
  return album_grid_new (FALSE);
}

SpotifyGtkAlbumGrid *
spotifygtk_album_grid_new_grid (void)
{
  return album_grid_new (TRUE);
}

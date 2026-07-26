/*
 * album_grid.c — A virtualised grid or shelf of album cards.
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
 * WHY THIS IS VIRTUALISED (it was not, and that was a memory leak in effect)
 *
 * The first version built one real widget tree per album into a GtkFlowBox or
 * GtkBox. The Library page asks for every distinct album in a 1000-track
 * collection — several hundred cards — and each card's GtkImage holds its own
 * reference on the decoded cover texture. The cover cache is bounded (48
 * entries) but that bound frees nothing while a widget still references the
 * texture, so evicting simply handed ownership to the cards. At 180px a cover
 * is ~126 KB, so ~600 live cards pinned ~75 MB of texture that could never be
 * reclaimed, on top of ~3000 widgets. That is the memory that "kept piling up".
 *
 * Now the albums live in a GListStore and the view builds only the cards it can
 * actually show (plus a small buffer), recycling them as you scroll — the same
 * treatment track_list.c already had. unbind drops the texture reference, so
 * scrolled-away covers become reclaimable and the retained set is bounded by
 * the viewport rather than by the size of the collection.
 *
 * Two shapes from one widget: `wrap` TRUE is a GtkGridView (the Library grid);
 * FALSE is a horizontal GtkListView (a Home/Search shelf). Both sit in their
 * own GtkScrolledWindow, which is what gives the view a viewport to virtualise
 * against — without one it would be handed unbounded space and realise every
 * item, defeating the point.
 */

#include "album_grid.h"
#include "cover_loader.h"

#define CARD_ART_PX     150   /* on-screen card art size */
#define CARD_DECODE_PX  180   /* decode a touch larger so downscaling stays crisp */
#define CARD_WIDTH      (CARD_ART_PX + 24)
#define SHELF_HEIGHT    236   /* art + two text lines + margins + scrollbar lane */

/* === One album in the model === */

#define SPOTIFYGTK_TYPE_ALBUM_ITEM (spotifygtk_album_item_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkAlbumItem, spotifygtk_album_item,
                      SPOTIFYGTK, ALBUM_ITEM, GObject)

struct _SpotifyGtkAlbumItem {
  GObject  parent_instance;
  gchar   *uri;
  gchar   *name;
  gchar   *artist;
  gchar   *cover_id;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkAlbumItem, spotifygtk_album_item, G_TYPE_OBJECT)

static void
spotifygtk_album_item_finalize (GObject *object)
{
  SpotifyGtkAlbumItem *self = SPOTIFYGTK_ALBUM_ITEM (object);
  g_free (self->uri);
  g_free (self->name);
  g_free (self->artist);
  g_free (self->cover_id);
  G_OBJECT_CLASS (spotifygtk_album_item_parent_class)->finalize (object);
}

static void
spotifygtk_album_item_class_init (SpotifyGtkAlbumItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = spotifygtk_album_item_finalize;
}

static void
spotifygtk_album_item_init (SpotifyGtkAlbumItem *self)
{
  (void) self;
}

static SpotifyGtkAlbumItem *
album_item_new (const gchar *uri, const gchar *name,
                const gchar *artist, const gchar *cover_id)
{
  SpotifyGtkAlbumItem *item = g_object_new (SPOTIFYGTK_TYPE_ALBUM_ITEM, NULL);
  item->uri      = g_strdup (uri);
  item->name     = g_strdup (name);
  item->artist   = g_strdup (artist);
  item->cover_id = g_strdup (cover_id);
  return item;
}

/* === The grid === */

struct _SpotifyGtkAlbumGrid {
  GtkBox parent_instance;

  GListStore *store;
  gboolean    wrap;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkAlbumGrid, spotifygtk_album_grid, GTK_TYPE_BOX)

enum { ALBUM_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* A pending cover load is cancelled when its card is recycled or destroyed, so
 * a late decode never paints onto a card now showing a different album. */
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
  if (uri && *uri)
    g_signal_emit (self, signals[ALBUM_ACTIVATED], 0, uri, name);
}

/* Build the reusable card shell once per recycled widget, not once per album. */
static void
factory_setup (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  SpotifyGtkAlbumGrid *self = user_data;

  GtkWidget *card = gtk_button_new ();
  gtk_widget_add_css_class (card, "media-card");
  gtk_widget_add_css_class (card, "flat");
  gtk_button_set_has_frame (GTK_BUTTON (card), FALSE);
  gtk_widget_set_size_request (card, CARD_WIDTH, -1);
  gtk_widget_set_valign (card, GTK_ALIGN_START);
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

  GtkWidget *title = gtk_label_new ("");
  gtk_widget_add_css_class (title, "media-card-title");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (title), 14);
  gtk_box_append (GTK_BOX (box), title);

  GtkWidget *sub = gtk_label_new ("");
  gtk_widget_add_css_class (sub, "media-card-subtitle");
  gtk_label_set_xalign (GTK_LABEL (sub), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (sub), 16);
  gtk_box_append (GTK_BOX (box), sub);

  g_object_set_data (G_OBJECT (card), "art",   art);
  g_object_set_data (G_OBJECT (card), "title", title);
  g_object_set_data (G_OBJECT (card), "sub",   sub);

  gtk_button_set_child (GTK_BUTTON (card), box);
  gtk_list_item_set_child (list_item, card);
  (void) factory;
}

static void
factory_bind (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  GtkWidget           *card = gtk_list_item_get_child (list_item);
  SpotifyGtkAlbumItem *item = gtk_list_item_get_item (list_item);
  if (!card || !item)
    return;

  GtkWidget *art   = g_object_get_data (G_OBJECT (card), "art");
  GtkWidget *title = g_object_get_data (G_OBJECT (card), "title");
  GtkWidget *sub   = g_object_get_data (G_OBJECT (card), "sub");

  gtk_label_set_text (GTK_LABEL (title), item->name ? item->name : "Unknown album");
  gtk_label_set_text (GTK_LABEL (sub), item->artist ? item->artist : "");
  gtk_widget_set_visible (sub, item->artist && *item->artist);

  g_object_set_data_full (G_OBJECT (card), "album-uri", g_strdup (item->uri), g_free);
  g_object_set_data_full (G_OBJECT (card), "album-name", g_strdup (item->name), g_free);

  /* Reset to the placeholder first: this card may still be showing the cover of
   * whichever album it was last bound to. */
  gtk_image_set_from_icon_name (GTK_IMAGE (art), "media-optical-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (art), CARD_ART_PX);

  if (item->cover_id && *item->cover_id) {
    GCancellable *cancel = g_cancellable_new ();
    g_object_set_data_full (G_OBJECT (card), "cover-cancel", cancel, cancel_and_unref);
    spotifygtk_cover_load (item->cover_id, CARD_DECODE_PX, cancel,
                           on_card_cover_loaded, art);
  }
  (void) factory; (void) user_data;
}

static void
factory_unbind (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  GtkWidget *card = gtk_list_item_get_child (list_item);
  if (!card)
    return;

  /* Clearing the data fires cancel_and_unref, stopping an in-flight decode. */
  g_object_set_data (G_OBJECT (card), "cover-cancel", NULL);

  /* Drop this card's reference on the texture. Without it a recycled card would
   * keep the old cover alive, and the retained set would grow with everything
   * ever scrolled past rather than with what is on screen. */
  GtkWidget *art = g_object_get_data (G_OBJECT (card), "art");
  if (art) {
    gtk_image_set_from_icon_name (GTK_IMAGE (art), "media-optical-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (art), CARD_ART_PX);
  }
  (void) factory; (void) user_data;
}

void
spotifygtk_album_grid_clear (SpotifyGtkAlbumGrid *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self));
  g_list_store_remove_all (self->store);
}

guint
spotifygtk_album_grid_set_from_tracks (SpotifyGtkAlbumGrid *self,
                                       GPtrArray           *tracks,
                                       guint                max_albums)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self), 0);

  g_list_store_remove_all (self->store);

  if (!tracks || tracks->len == 0)
    return 0;

  /* Distinct albums in first-seen order, so the cards line up with the order of
   * the track list they came from. Keys are borrowed from the live tracks
   * array, which outlives this call. */
  g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  guint shown = 0;

  for (guint i = 0; i < tracks->len && shown < max_albums; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (tracks, i);
    if (!t || !t->album_uri || !*t->album_uri)
      continue;
    if (g_hash_table_contains (seen, t->album_uri))
      continue;
    g_hash_table_add (seen, t->album_uri);

    g_autoptr(SpotifyGtkAlbumItem) item =
      album_item_new (t->album_uri, t->album, t->artists, t->cover_id);
    g_list_store_append (self->store, item);
    shown++;
  }

  return shown;
}

static void
spotifygtk_album_grid_dispose (GObject *object)
{
  SpotifyGtkAlbumGrid *self = SPOTIFYGTK_ALBUM_GRID (object);
  g_clear_object (&self->store);
  G_OBJECT_CLASS (spotifygtk_album_grid_parent_class)->dispose (object);
}

static void
spotifygtk_album_grid_class_init (SpotifyGtkAlbumGridClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_album_grid_dispose;

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
  self->wrap  = wrap;
  self->store = g_list_store_new (SPOTIFYGTK_TYPE_ALBUM_ITEM);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup",  G_CALLBACK (factory_setup),  self);
  g_signal_connect (factory, "bind",   G_CALLBACK (factory_bind),   self);
  g_signal_connect (factory, "unbind", G_CALLBACK (factory_unbind), self);

  /* No-selection: cards are buttons, not a picker, so there is no selection
   * highlight to manage. */
  GtkNoSelection *model =
    gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->store)));

  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *view;

  if (wrap) {
    view = gtk_grid_view_new (GTK_SELECTION_MODEL (model), factory);
    gtk_grid_view_set_min_columns (GTK_GRID_VIEW (view), 2);
    gtk_grid_view_set_max_columns (GTK_GRID_VIEW (view), 8);
    gtk_widget_add_css_class (view, "album-gridview");

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (scroller, TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  } else {
    view = gtk_list_view_new (GTK_SELECTION_MODEL (model), factory);
    gtk_orientable_set_orientation (GTK_ORIENTABLE (view),
                                    GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class (view, "album-shelfview");
    /* Leave a lane below the cards so the overlay scrollbar rides in empty
     * space rather than across the artist labels. */
    gtk_widget_set_margin_bottom (view, 14);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroller),
                                                SHELF_HEIGHT);
  }

  gtk_widget_set_hexpand (scroller, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), view);
  gtk_box_append (GTK_BOX (self), scroller);

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

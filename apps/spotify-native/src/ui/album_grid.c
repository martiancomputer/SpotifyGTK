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
#include "context_menu.h"
#include "cover_loader.h"
#include "smooth_scroll.h"

#define CARD_ART_PX     176   /* on-screen card art size */
/*
 * Decode at roughly 2x the on-screen size. At 180 against a 150px card there
 * was almost no headroom, so any downscaling GTK did landed on nearly
 * one-to-one pixels and the art looked soft -- and on a HiDPI display the
 * card is physically 2x its logical size, so the source was genuinely below
 * the panel resolution. Spotify already hands us the largest variant it has
 * (see dup_largest_cover_id), so this costs download bandwidth we were
 * spending anyway; what it does cost is texture memory, ~4x per cached cover.
 */
/*
 * Decode size for a card.
 *
 * Twice the nominal art size, and that headroom is not slack. gtk_image's
 * pixel-size governs icons; a paintable is scaled to the widget's allocation
 * instead, and cards stretch to fill their column, so the art is routinely
 * drawn wider than CARD_ART_PX. Decoding at exactly that size left nothing to
 * downscale from and the covers came out soft.
 *
 * The scale factor is folded in on top, so a HiDPI panel gets real pixels
 * rather than the 1x assumption the old fixed 352 made.
 *
 * This is the expensive choice -- texture memory goes as the square, and
 * covers are the largest thing this process keeps. It is made deliberately:
 * the way to spend less here is a disk cache, so evicting costs a decode
 * rather than a download, not decoding art too small to look right.
 */
static gint
card_decode_px (GtkWidget *widget)
{
  gint scale = widget ? gtk_widget_get_scale_factor (widget) : 1;
  return CARD_ART_PX * 2 * MAX (1, scale);
}
#define CARD_WIDTH      (CARD_ART_PX + 24)

/* Vertical space a shelf card needs beyond the art itself: 8+8 box margins,
 * two 8px gaps, a title row and an artist row, plus the button's own padding
 * and a few pixels of clearance so the scrollbar underneath is not touching
 * the text. */
#define SHELF_TEXT_ALLOWANCE  96
/* Strip the horizontal scrollbar occupies below the cards. */
#define SHELF_BAR_ALLOWANCE   20

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

  /* A card added URI-only, still waiting for its name and cover. See
   * spotifygtk_album_grid_add_pending_card(). */
  gboolean pending;
  gboolean resolving;   /* a request is out; do not ask again on every rebind */
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

  GtkWidget  *scroller;   /* borrowed: owned by the box */
  GtkWidget  *view;       /* borrowed: owned by the scroller */

  /* Scroll settle detection, mirroring track_list. Without it a fling through
   * the grid fires a request for every card it passes -- each a 352px decode
   * roughly thirteen times the size of a row cover, so a few hundred of them
   * evict the entire cache and everything after misses. This is the page where
   * covers took seconds to appear after scrolling far. */
  GtkAdjustment *vadj;      /* borrowed */
  guint          settle_id;
  GPtrArray     *bound_cards;   /* borrowed GtkWidget*, live bindings */

};

G_DEFINE_FINAL_TYPE (SpotifyGtkAlbumGrid, spotifygtk_album_grid, GTK_TYPE_BOX)

enum { ALBUM_ACTIVATED, ALBUM_ADD_TO_LIKED, ALBUM_ADD_TO_QUEUE,
       CARD_NEEDS_RESOLVE, N_SIGNALS };
static guint signals[N_SIGNALS];

#define GRID_SETTLE_MS 180

static void cancel_and_unref (gpointer data);
static void on_card_cover_loaded (GdkTexture *texture, gpointer user_data);

/* Re-request the cover for one card if its load was skipped while scrolling. */
static void
card_retry_cover (GtkWidget *card)
{
  const gchar *cover_id = g_object_get_data (G_OBJECT (card), "cover-id");
  GtkWidget   *art      = g_object_get_data (G_OBJECT (card), "art");
  if (!cover_id || !art)
    return;
  if (g_object_get_data (G_OBJECT (card), "cover-shown"))
    return;

  GCancellable *cancel = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (card), "cover-cancel", cancel, cancel_and_unref);
  spotifygtk_cover_load_deferrable (cover_id, card_decode_px (card), cancel,
                                    on_card_cover_loaded, art);
}

static gboolean
on_grid_settled (gpointer user_data)
{
  SpotifyGtkAlbumGrid *self = user_data;
  self->settle_id = 0;

  spotifygtk_cover_set_deferred (FALSE);
  for (guint i = 0; i < self->bound_cards->len; i++)
    card_retry_cover (g_ptr_array_index (self->bound_cards, i));

  return G_SOURCE_REMOVE;
}

static void
on_grid_scrolled (GtkAdjustment *adj, gpointer user_data)
{
  SpotifyGtkAlbumGrid *self = user_data;
  (void) adj;

  spotifygtk_cover_set_deferred (TRUE);
  if (self->settle_id)
    g_source_remove (self->settle_id);
  self->settle_id = g_timeout_add (GRID_SETTLE_MS, on_grid_settled, self);
}

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
  if (!texture)
    return;   /* keep the placeholder; the settle pass may come back for it */

  gtk_image_set_from_paintable (art, GDK_PAINTABLE (texture));

  GtkWidget *card = gtk_widget_get_ancestor (GTK_WIDGET (art), GTK_TYPE_BUTTON);
  if (card)
    g_object_set_data (G_OBJECT (card), "cover-shown", GINT_TO_POINTER (1));
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

/* === Right-click context menu ===
 *
 * Mirrors the track row menu so the two behave the same way. The card is a
 * GtkButton, so a secondary-click gesture has to claim the sequence or the
 * button swallows it and activates the album instead.
 *
 * The album's identity is copied into the menu's context: a card is recycled
 * as the grid scrolls, and the menu can outlive the card it came from.
 */
typedef struct {
  SpotifyGtkAlbumGrid *grid;
  gchar               *uri;
  gchar               *name;
} AlbumMenuCtx;

static void
album_menu_ctx_free (gpointer data)
{
  AlbumMenuCtx *ctx = data;
  g_free (ctx->uri);
  g_free (ctx->name);
  g_free (ctx);
}

static void
album_menu_emit_and_close (GtkButton *button, guint signal_id)
{
  GtkWidget *w = GTK_WIDGET (button);
  AlbumMenuCtx *ctx = spotifygtk_context_menu_get_context (w);
  if (ctx)
    g_signal_emit (ctx->grid, signals[signal_id], 0, ctx->uri);

  GtkPopover *popover = spotifygtk_context_menu_get_popover (w);
  if (popover)
    gtk_popover_popdown (popover);
}

static void on_album_menu_like  (GtkButton *b, gpointer d) { (void) d; album_menu_emit_and_close (b, ALBUM_ADD_TO_LIKED); }
static void on_album_menu_queue (GtkButton *b, gpointer d) { (void) d; album_menu_emit_and_close (b, ALBUM_ADD_TO_QUEUE); }

static void
on_card_secondary_pressed (GtkGestureClick *gesture, gint n_press,
                           gdouble x, gdouble y, gpointer user_data)
{
  SpotifyGtkAlbumGrid *self = user_data;
  GtkWidget *card = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

  const gchar *uri = g_object_get_data (G_OBJECT (card), "album-uri");
  if (!uri)
    return;   /* card not bound to an album yet */

  AlbumMenuCtx *ctx = g_new0 (AlbumMenuCtx, 1);
  ctx->grid = self;
  ctx->uri  = g_strdup (uri);
  ctx->name = g_strdup (g_object_get_data (G_OBJECT (card), "album-name"));

  SpotifyGtkContextMenu *menu = spotifygtk_context_menu_new ();
  spotifygtk_context_menu_add (menu, "Add to Liked Songs", TRUE, NULL,
                               G_CALLBACK (on_album_menu_like), NULL);
  spotifygtk_context_menu_add (menu, "Add to Queue", TRUE, NULL,
                               G_CALLBACK (on_album_menu_queue), NULL);
  /* The grid model carries the artist's display name but not its URI, so
   * there is nothing to navigate to yet. */
  spotifygtk_context_menu_add (menu, "Go to Artist", FALSE,
                               "The album list does not carry an artist id yet",
                               NULL, NULL);
  spotifygtk_context_menu_add_separator (menu);
  spotifygtk_context_menu_add (menu, "Share album", FALSE,
                               "Not implemented yet", NULL, NULL);

  spotifygtk_context_menu_present (menu, card, x, y, ctx, album_menu_ctx_free);

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  (void) n_press;
}

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

  /* Added once per pooled card; the handler reads whichever album is bound at
   * click time, so it follows the card through recycling. */
  GtkGesture *secondary = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (secondary), GDK_BUTTON_SECONDARY);
  g_signal_connect (secondary, "pressed", G_CALLBACK (on_card_secondary_pressed), self);
  gtk_widget_add_controller (card, GTK_EVENT_CONTROLLER (secondary));

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
  gtk_label_set_max_width_chars (GTK_LABEL (title), 18);
  gtk_box_append (GTK_BOX (box), title);

  GtkWidget *sub = gtk_label_new ("");
  gtk_widget_add_css_class (sub, "media-card-subtitle");
  gtk_label_set_xalign (GTK_LABEL (sub), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (sub), 20);
  gtk_box_append (GTK_BOX (box), sub);

  g_object_set_data (G_OBJECT (card), "art",   art);
  g_object_set_data (G_OBJECT (card), "title", title);
  g_object_set_data (G_OBJECT (card), "sub",   sub);

  gtk_button_set_child (GTK_BUTTON (card), box);
  gtk_list_item_set_child (list_item, card);
  (void) factory;
}

static void card_apply_item (SpotifyGtkAlbumGrid *self, GtkWidget *card,
                             SpotifyGtkAlbumItem *item);

static void
factory_bind (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  SpotifyGtkAlbumGrid *self = user_data;
  GtkWidget           *card = gtk_list_item_get_child (list_item);
  SpotifyGtkAlbumItem *item = gtk_list_item_get_item (list_item);
  if (!card || !item)
    return;

  card_apply_item (self, card, item);

  if (!g_ptr_array_find (self->bound_cards, card, NULL))
    g_ptr_array_add (self->bound_cards, card);

  /*
   * Ask for the details the moment the card is actually on screen.
   *
   * Resolving every entry up front cost one round trip each, serialised, for
   * cards the user may never scroll to. Only a dozen or so are visible at a
   * time, so this turns a fan-out over the whole listing into a handful.
   */
  if (item->pending && !item->resolving) {
    item->resolving = TRUE;
    g_signal_emit (self, signals[CARD_NEEDS_RESOLVE], 0, item->uri);
  }
  (void) factory;
}

/* Paint one item onto one card. Shared by bind and by resolve, so a card that
 * is already on screen when its details arrive updates the same way it would
 * have on a fresh bind. */
static void
card_apply_item (SpotifyGtkAlbumGrid *self, GtkWidget *card, SpotifyGtkAlbumItem *item)
{
  GtkWidget *art   = g_object_get_data (G_OBJECT (card), "art");
  GtkWidget *title = g_object_get_data (G_OBJECT (card), "title");
  GtkWidget *sub   = g_object_get_data (G_OBJECT (card), "sub");
  if (!art || !title || !sub)
    return;

  gtk_label_set_text (GTK_LABEL (title), item->name ? item->name : "Unknown album");
  gtk_label_set_text (GTK_LABEL (sub), item->artist ? item->artist : "");
  gtk_widget_set_visible (sub, item->artist && *item->artist);

  g_object_set_data_full (G_OBJECT (card), "album-uri", g_strdup (item->uri), g_free);
  g_object_set_data_full (G_OBJECT (card), "album-name", g_strdup (item->name), g_free);

  /* Reset to the placeholder first: this card may still be showing the cover of
   * whichever album it was last bound to. */
  gtk_image_set_from_icon_name (GTK_IMAGE (art), "media-optical-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (art), CARD_ART_PX);

  g_object_set_data (G_OBJECT (card), "cover-shown", NULL);
  g_object_set_data_full (G_OBJECT (card), "cover-id",
                          g_strdup (item->cover_id), g_free);

  if (item->cover_id && *item->cover_id) {
    GCancellable *cancel = g_cancellable_new ();
    g_object_set_data_full (G_OBJECT (card), "cover-cancel", cancel, cancel_and_unref);
    spotifygtk_cover_load_deferrable (item->cover_id, card_decode_px (card), cancel,
                                      on_card_cover_loaded, art);
  }
  (void) self;
}

static void
factory_unbind (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  SpotifyGtkAlbumGrid *self = user_data;
  GtkWidget *card = gtk_list_item_get_child (list_item);
  if (card && self->bound_cards)
    g_ptr_array_remove_fast (self->bound_cards, card);
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

/*
 * A card that knows only its URI. Its name and cover are fetched when it first
 * scrolls into view -- see the CARD_NEEDS_RESOLVE emission in factory_bind().
 */
void
spotifygtk_album_grid_add_pending_card (SpotifyGtkAlbumGrid *self, const gchar *uri,
                                        const gchar *placeholder_title,
                                        const gchar *subtitle)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self));
  g_return_if_fail (uri != NULL);

  g_autoptr(SpotifyGtkAlbumItem) item =
    album_item_new (uri, placeholder_title, subtitle, NULL);
  item->pending = TRUE;
  g_list_store_append (self->store, item);
}

/*
 * Fill in a pending card's details.
 *
 * Sets the fields on the item and repaints whichever card is currently showing
 * it, if any. Deliberately not a splice: replacing the item would destroy the
 * one a card is bound to and force a rebind with cover loads still in flight
 * against that card, which is what crashed when cards were patched before.
 * Updating in place touches no model structure at all.
 *
 * A card scrolled out of view has nothing to repaint, and does not need one --
 * the item holds the data, so it is correct on the next bind.
 */
void
spotifygtk_album_grid_resolve_card (SpotifyGtkAlbumGrid *self, const gchar *uri,
                                    const gchar *title, const gchar *subtitle,
                                    const gchar *cover_id)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self));
  g_return_if_fail (uri != NULL);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (guint i = 0; i < n; i++) {
    g_autoptr(SpotifyGtkAlbumItem) item =
      g_list_model_get_item (G_LIST_MODEL (self->store), i);
    if (!item || g_strcmp0 (item->uri, uri) != 0)
      continue;

    g_free (item->name);     item->name     = g_strdup (title);
    g_free (item->artist);   item->artist   = g_strdup (subtitle);
    g_free (item->cover_id); item->cover_id = g_strdup (cover_id);
    item->pending   = FALSE;
    item->resolving = FALSE;

    for (guint c = 0; c < self->bound_cards->len; c++) {
      GtkWidget *card = g_ptr_array_index (self->bound_cards, c);
      const gchar *shown = g_object_get_data (G_OBJECT (card), "album-uri");
      if (g_strcmp0 (shown, uri) == 0)
        card_apply_item (self, card, item);
    }
    return;
  }
}

/*
 * Drop the artwork of every bound card.
 *
 * A card holds a reference to its texture, so the cover cache cannot free one
 * that any card is still showing -- including cards on a page the user
 * navigated away from, which GTK keeps realised. Measured on a full library,
 * every cached cover sat at three references: the cache, the widget, and the
 * renderer. Clearing the widget's is what actually lets the memory go.
 *
 * The cover id is left in place, so scrolling back reloads it.
 */
void
spotifygtk_album_grid_release_covers (SpotifyGtkAlbumGrid *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self));
  if (!self->bound_cards)
    return;

  for (guint i = 0; i < self->bound_cards->len; i++) {
    GtkWidget *card = g_ptr_array_index (self->bound_cards, i);
    GtkWidget *art  = g_object_get_data (G_OBJECT (card), "art");
    if (!art)
      continue;
    g_object_set_data (G_OBJECT (card), "cover-cancel", NULL);   /* cancels in flight */
    g_object_set_data (G_OBJECT (card), "cover-shown", NULL);
    gtk_image_set_from_icon_name (GTK_IMAGE (art), "media-optical-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (art), CARD_ART_PX);
  }
}

void
spotifygtk_album_grid_clear (SpotifyGtkAlbumGrid *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self));
  g_list_store_remove_all (self->store);
}


/*
 * Add one card directly, rather than deriving it from a track list.
 *
 * Playlists are not albums: they have no album URI to group by, and their
 * cover is not carried anywhere in the playlist itself, so the caller resolves
 * both separately and supplies the finished pieces.
 *
 * There is deliberately no way to patch a card after adding it. That meant
 * splicing the model, which destroys the item a bound card is showing and
 * forces a rebind while a cover load may still hold a borrowed pointer to that
 * card. It crashed when a playlist card was clicked while its art arrived.
 * Resolve first, add once.
 */
void
spotifygtk_album_grid_add_card (SpotifyGtkAlbumGrid *self, const gchar *uri,
                                const gchar *title, const gchar *subtitle,
                                const gchar *cover_id)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self));
  g_return_if_fail (uri != NULL);

  g_autoptr(SpotifyGtkAlbumItem) item =
    album_item_new (uri, title ? title : uri, subtitle ? subtitle : "",
                               cover_id);
  g_list_store_append (self->store, item);
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

  /* Collected first and spliced in one go. Appending per item emits
   * items-changed each time and the view revalidates on every one, which on a
   * library of several hundred albums is several hundred rounds of work to
   * show one list. */
  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < tracks->len && items->len < max_albums; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (tracks, i);
    if (!t || !t->album_uri || !*t->album_uri)
      continue;
    if (g_hash_table_contains (seen, t->album_uri))
      continue;
    g_hash_table_add (seen, t->album_uri);

    g_ptr_array_add (items, album_item_new (t->album_uri, t->album,
                                            t->artists, t->cover_id));
  }

  guint existing = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  g_list_store_splice (self->store, 0, existing, items->pdata, items->len);

  return items->len;
}

static void
spotifygtk_album_grid_dispose (GObject *object)
{
  SpotifyGtkAlbumGrid *self = SPOTIFYGTK_ALBUM_GRID (object);

  /* The settle timer holds a plain pointer to this grid, and deferral is
   * global -- left set, the next page built would treat every miss as
   * skippable and show no art at all. */
  if (self->settle_id) {
    g_source_remove (self->settle_id);
    self->settle_id = 0;
  }
  spotifygtk_cover_set_deferred (FALSE);
  g_clear_pointer (&self->bound_cards, g_ptr_array_unref);

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

  /* Both carry the album URI. Nothing is connected yet: the collection write
   * behind "Add to Liked Songs" is not implemented, and queueing an album
   * needs its track list resolved first. The menu is wired so the plumbing
   * can be exercised before either lands. */
  signals[ALBUM_ADD_TO_LIKED] = g_signal_new (
    "album-add-to-liked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  /* Emitted the first time a pending card is bound, i.e. scrolled into view.
   * The handler is expected to fetch its details and call resolve_card(). */
  signals[CARD_NEEDS_RESOLVE] = g_signal_new (
    "card-needs-resolve", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_FIRST, 0,
    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[ALBUM_ADD_TO_QUEUE] = g_signal_new (
    "album-add-to-queue", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
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
    /*
     * Non-overlay, matching every other scroller in the app. Overlay was tried
     * to reclaim the right-hand gutter and was the wrong trade: GTK draws an
     * overlay bar with a translucent trough that floats over the content, so
     * this one bar no longer looked like any of the others. The gutter is a
     * margin problem and is fixed as one -- see set_content_margins(), which
     * insets the cards while leaving the scroller itself flush right.
     */
    gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);
    gtk_widget_set_vexpand (scroller, TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

    spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (scroller),
                                     GTK_ORIENTATION_VERTICAL);
  } else {
    view = gtk_list_view_new (GTK_SELECTION_MODEL (model), factory);
    gtk_orientable_set_orientation (GTK_ORIENTABLE (view),
                                    GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class (view, "album-shelfview");

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    /* The bar gets its own strip under the cards rather than being drawn across
     * the artist line. */
    gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);

    /*
     * Two things together, because natural-height propagation alone was not
     * enough: it raises the *natural* request, but a GtkBox under pressure
     * hands out minimum heights, and the minimum here is far below a card. The
     * cards stayed clipped -- title cut through its descenders, artist line
     * gone entirely, scrollbar sitting in the wound.
     *
     * min_content_height is the part a parent must honour. Sized from the art
     * plus a measured allowance for the two text rows, margins and the button's
     * padding, with SHELF_TEXT_ALLOWANCE deliberately generous: being a few
     * pixels over costs a few pixels of whitespace, being a few pixels under
     * costs a clipped title, and those are not symmetric mistakes.
     */
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroller), TRUE);
    gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroller),
                                                CARD_ART_PX + SHELF_TEXT_ALLOWANCE);

    /*
     * And a hard floor on the widget itself, because the two settings above
     * were not enough on Home.
     *
     * On the scroller, not on the SpotifyGtkAlbumGrid box that wraps it. That
     * distinction is the entire bug: this widget is a vertical GtkBox, and the
     * shelf branch -- unlike the grid branch above -- never sets vexpand on its
     * scroller. A floor on the box therefore made the *box* 292px while the
     * scroller inside kept taking only its own natural height, so the dead
     * space appeared below the scrollbar and the cards stayed exactly as
     * squeezed as before. Constrain the thing whose height actually matters.
     */
    gtk_widget_set_size_request (scroller, -1,
                                 CARD_ART_PX + SHELF_TEXT_ALLOWANCE + SHELF_BAR_ALLOWANCE);

    spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (scroller),
                                     GTK_ORIENTATION_HORIZONTAL);
  }

  gtk_widget_set_hexpand (scroller, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), view);
  gtk_box_append (GTK_BOX (self), scroller);

  self->scroller = scroller;
  self->view     = view;

  self->bound_cards = g_ptr_array_new ();
  self->vadj = wrap
    ? gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller))
    : gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (scroller));
  if (self->vadj)
    g_signal_connect (self->vadj, "value-changed",
                      G_CALLBACK (on_grid_scrolled), self);

  return self;
}

/* The scrolling adjustment, so a page can react to scroll position -- the
 * Library uses it to fold its header away. NULL before the view is built. */
GtkAdjustment *
spotifygtk_album_grid_get_vadjustment (SpotifyGtkAlbumGrid *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self), NULL);
  if (!self->scroller)
    return NULL;
  return gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scroller));
}

/*
 * Inset the cards without insetting the scrollbar.
 *
 * A page that puts its own margin on this whole widget pushes the scrollbar
 * inward along with the content, which stacks the bar's width on top of the
 * page margin and leaves a dead gutter -- ~80px against ~35px on the other
 * side, which is what it looked like. Applying the inset to the view instead
 * keeps the bar flush with the page edge, where every other scroller in the
 * app puts it, and lets the cards sit at whatever margin the page wants.
 */
void
spotifygtk_album_grid_set_content_margins (SpotifyGtkAlbumGrid *self,
                                           int start, int end)
{
  g_return_if_fail (SPOTIFYGTK_IS_ALBUM_GRID (self));
  if (!self->view)
    return;
  gtk_widget_set_margin_start (self->view, start);
  gtk_widget_set_margin_end (self->view, end);
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

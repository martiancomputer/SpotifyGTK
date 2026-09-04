/*
 * artist_page.c — see artist_page.h.
 */

#include "artist_page.h"

#include "cover_loader.h"
#include "smooth_scroll.h"

#include <string.h>

/*
 * How much of the artist's context to resolve, and how much of it to show as
 * "most played".
 *
 * The resolve is the only request this page makes, and everything on it comes
 * out of that one list -- the tracks at the top and the releases below -- so
 * the limit is set by wanting enough albums to be worth grouping rather than
 * by the dozen rows on show.
 */
#define ARTIST_PAGE_LIMIT   200
#define ARTIST_TOP_TRACKS   12

/*
 * Height of the banner, and it is enforced rather than requested.
 *
 * set_size_request only sets a minimum. A GtkPicture reports the texture's
 * own size as its natural size, so a box holding one grows to the image:
 * asking for 320 and measuring 511 is what that looked like. Inside a
 * scrolled window, which hands out natural height, nothing pushed back.
 *
 * Hence the overlay below -- an overlay child that is not a measure-overlay
 * contributes nothing to measurement and is allocated the overlay's size, so
 * the sizer decides the height and the picture fills whatever it is given.
 */
/* The scrolling content's right margin. The fixed header adds the scrollbar
 * gutter to this so both edges line up -- see align_title_to_hero(). */
#define CONTENT_MARGIN_END  12

#define HERO_HEIGHT         300
/*
 * Decode size for the banner.
 *
 * The loader fits an image inside target x target preserving aspect, so for a
 * header -- wide, typically around 21:9 -- the target sets the *width*. 640
 * was the album-cover figure and far too small: the panel is as wide as the
 * page, so a 640px decode was being scaled up about twice and looked it.
 *
 * 1280 covers a maximised window on a 1080p display, and the scale factor is
 * folded in for HiDPI, exactly as the album cards do it. Only ever one of
 * these is held at a time, so the cost is one image rather than a gridful.
 */
#define HERO_IMAGE_PX       1280

/*
 * Release kinds.
 *
 * Exact, not inferred. These come from Album.type in the catalogue, which the
 * discography load reads directly -- an earlier version guessed from how many
 * of the artist's tracks a release contributed, because a context resolve
 * returns tracks rather than album objects and the type never reached the
 * page. It reaches it now, so a two-track EP is an EP.
 *
 * These sort, they do not filter. The heading says "Albums, EPs and Singles",
 * so all three are always on the page and the buttons decide which comes
 * first -- exactly what the sort on Liked Songs does.
 *
 * "All" is the fourth key and the default: no kind is promoted and the
 * releases keep the order the catalogue gave them, which is roughly newest
 * first. It is not a filter reset -- nothing is ever hidden here -- it is the
 * way back to an unsorted page, which the other three had no way to restore.
 */
typedef enum {
  RELEASE_ALL = 0,
  RELEASE_ALBUM,
  RELEASE_EP,
  RELEASE_SINGLE,
  N_RELEASE_KINDS
} ReleaseKind;

static const gchar *const KIND_LABELS[N_RELEASE_KINDS] = {
  "All", "Album", "EP", "Singles"
};

/* One release out of the artist's discography. */
typedef struct {
  gchar           *uri;
  gchar           *name;
  gchar           *cover_id;
  GPtrArray       *tracks;    /* SpotifyNativeTrack*, owned copies */
  gint             year;
  SpotifyAlbumType type;      /* from the catalogue */
  guint            first_seen; /* keeps the catalogue's own order as the tiebreak */
} Release;

static void
release_free (gpointer data)
{
  Release *r = data;
  g_free (r->uri);
  g_free (r->name);
  g_free (r->cover_id);
  g_clear_pointer (&r->tracks, g_ptr_array_unref);
  g_free (r);
}

struct _SpotifyGtkArtistPage {
  GtkBox parent_instance;

  GtkLabel            *kind_label;
  GtkLabel            *title_label;
  GtkLabel            *year_label;
  GtkWidget           *follow_btn;
  GtkWidget           *title_row;   /* right edge kept level with the hero */
  GtkWidget           *scroller;
  SpotifyGtkArtistFollowFunc follow_fn;
  gpointer                   follow_data;

  GtkPicture          *hero_art;   /* a banner, not an icon -- see init */
  GtkLabel            *hero_caption;

  SpotifyGtkTrackList *list;
  GtkWidget           *releases_box;   /* one section per release */
  GtkLabel            *releases_status;
  GtkWidget           *kind_buttons[N_RELEASE_KINDS];
  ReleaseKind          sort_kind;
  gboolean             sort_desc[N_RELEASE_KINDS];

  GPtrArray           *all_releases;   /* Release*, in first-seen order */

  /* The window wires every list it owns for the row context menu; the release
   * sections create theirs after the fact, so it hands one in. */
  SpotifyGtkArtistListWireFunc wire_list;
  gpointer                     wire_data;

  SpotifyNativeSession *session;
  GCancellable         *in_flight;
  gchar                *current_uri;
  guint                 generation;
  guint                 pending_loads;
  gboolean              loading;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkArtistPage, spotifygtk_artist_page, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, LOADING_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct {
  GWeakRef page;
  guint    generation;
} ArtistLoad;

static void
set_loading (SpotifyGtkArtistPage *self, gboolean loading)
{
  loading = !!loading;
  if (self->loading == loading)
    return;
  self->loading = loading;
  g_signal_emit (self, signals[LOADING_CHANGED], 0, loading);
}

static void
finish_load_part (SpotifyGtkArtistPage *self)
{
  if (self->pending_loads > 0)
    self->pending_loads--;
  if (self->pending_loads == 0) {
    g_clear_object (&self->in_flight);
    set_loading (self, FALSE);
  }
}

/*
 * Written out rather than casting gtk_image_set_from_paintable to the callback
 * type: it takes (image, paintable) and the callback delivers (texture,
 * user_data), so the cast passes the texture as the image and GTK rejects it.
 */
static void
on_hero_cover_loaded (GdkTexture *texture, gpointer user_data)
{
  GtkPicture *pic = user_data;
  if (!texture || !GTK_IS_PICTURE (pic))
    return;

  /*
   * Only a landscape image gets covered into the banner. What arrives is not
   * always one: the true header is unreachable (see session.c), so this is
   * sometimes a promo photo and sometimes the square-ish avatar, and covering
   * a 0.84:1 avatar across a 4.5:1 strip magnifies a face until it is a crop
   * of a cheek. Fitting it instead leaves the panel colour either side, which
   * reads as a portrait on a backdrop rather than as a mistake.
   */
  gint w = gdk_texture_get_width (texture);
  gint h = gdk_texture_get_height (texture);
  gtk_picture_set_content_fit (pic, (h > 0 && (gdouble) w / h >= 1.2)
                                      ? GTK_CONTENT_FIT_COVER
                                      : GTK_CONTENT_FIT_CONTAIN);
  gtk_picture_set_paintable (pic, GDK_PAINTABLE (texture));
}

/* The portrait, once the extended-metadata request answers. */
static void
on_artist_image (const gchar *cover_id, gpointer user_data)
{
  g_autoptr(SpotifyGtkArtistPage) self = g_weak_ref_get (user_data);
  g_weak_ref_clear (user_data);
  g_free (user_data);

  if (!self)
    return;

  if (!cover_id || !*cover_id) {
    /* Neither a header nor a portrait. The placeholder stays, and the caption
     * says so rather than leaving an unexplained empty panel. */
    gtk_label_set_text (self->hero_caption, "No artist image");
    return;
  }

  gtk_label_set_text (self->hero_caption, "");
  /* Asked for at the banner's width, not its height: the loader fits within a
   * square, so for a wide image the target is what the width becomes. The
   * banner is ~2660x1140, so this decodes to about 1280x549 -- enough to draw
   * a full-width panel without upscaling, and a decode box much past it is
   * megabytes of pixels for no visible gain. */
  gint scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
  spotifygtk_cover_load (cover_id, HERO_IMAGE_PX * MAX (1, scale), NULL,
                         on_hero_cover_loaded, self->hero_art);
}

static void on_follow_clicked (GtkButton *button, gpointer user_data);

/*
 * Keep the header's right edge level with the hero's.
 *
 * The title row is fixed above the scroller, while the hero lives inside it in
 * a box with a margin -- so the hero is inset by that margin *plus* the
 * scrollbar gutter, and the Follow button was not. Measured rather than
 * hardcoded: the gutter is whatever the scrollbar is actually allocated, and
 * the margin is the one the content box carries.
 */
static gboolean
align_title_to_hero (GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
  SpotifyGtkArtistPage *self = data;
  (void) w; (void) clock;

  if (!self->title_row || !self->scroller)
    return G_SOURCE_REMOVE;

  GtkWidget *vsb = gtk_scrolled_window_get_vscrollbar (
    GTK_SCROLLED_WINDOW (self->scroller));
  gint gutter = vsb ? gtk_widget_get_width (vsb) : 0;

  /* Not laid out yet; come back next frame rather than guess. */
  if (gutter <= 0)
    return G_SOURCE_CONTINUE;

  gint want = gutter + CONTENT_MARGIN_END;
  if (gtk_widget_get_margin_end (self->title_row) != want) {
    gtk_widget_set_margin_end (self->title_row, want);
    return G_SOURCE_CONTINUE;   /* verify after it has been laid out */
  }
  return G_SOURCE_REMOVE;
}

static void
on_follow_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  (void) button;
  if (self->follow_fn && self->current_uri)
    self->follow_fn (self->current_uri, self->follow_data);
}

void
spotifygtk_artist_page_set_follow_handler (SpotifyGtkArtistPage      *self,
                                           SpotifyGtkArtistFollowFunc fn,
                                           gpointer                   user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  self->follow_fn   = fn;
  self->follow_data = user_data;
}

void
spotifygtk_artist_page_set_following (SpotifyGtkArtistPage *self,
                                      gboolean following)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  if (!self->follow_btn)
    return;
  gtk_button_set_label (GTK_BUTTON (self->follow_btn),
                        following ? "Following" : "Follow");
  gtk_widget_set_visible (self->follow_btn, TRUE);
}

static void
on_track_activated (SpotifyGtkTrackList *list, gpointer track, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  (void) list;
  g_signal_emit (self, signals[TRACK_ACTIVATED], 0, track);
}


static ReleaseKind
release_kind_of (const Release *r)
{
  switch (r->type) {
    case SPOTIFY_ALBUM_TYPE_SINGLE:      return RELEASE_SINGLE;
    case SPOTIFY_ALBUM_TYPE_EP:          return RELEASE_EP;
    case SPOTIFY_ALBUM_TYPE_ALBUM:
    case SPOTIFY_ALBUM_TYPE_COMPILATION: return RELEASE_ALBUM;
    default: break;
  }
  /* Only for a release the catalogue gave no type at all. */
  return (r->tracks && r->tracks->len <= 2) ? RELEASE_SINGLE : RELEASE_ALBUM;
}

/* Chosen kind first; everything else keeps the order the resolve gave it. */
static gint
compare_releases (gconstpointer a, gconstpointer b, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  const Release *x = *(const Release **) a;
  const Release *y = *(const Release **) b;

  /* "All" promotes nothing; the tiebreak below is the whole order. */
  if (self->sort_kind != RELEASE_ALL) {
    gboolean xf = (release_kind_of (x) == self->sort_kind);
    gboolean yf = (release_kind_of (y) == self->sort_kind);
    if (xf != yf)
      return self->sort_desc[self->sort_kind] ? (xf - yf) : (yf - xf);
  } else if (self->sort_desc[RELEASE_ALL]) {
    return (x->first_seen < y->first_seen) - (x->first_seen > y->first_seen);
  }

  /* Stable within a group: the resolve's own order, which is Spotify's. */
  return (x->first_seen > y->first_seen) - (x->first_seen < y->first_seen);
}

/*
 * Rebuild the release sections in the current order.
 *
 * Every release is expanded in place -- its name, then its tracks -- and they
 * stack downwards, so the whole discography reads by scrolling. Cards would
 * mean a click into each release and a click back out to reach the next, which
 * is the friction this page exists to remove.
 */
static void
apply_release_sort (SpotifyGtkArtistPage *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->releases_box)) != NULL)
    gtk_box_remove (GTK_BOX (self->releases_box), child);

  if (!self->all_releases)
    return;

  g_autoptr(GPtrArray) ordered = g_ptr_array_sized_new (self->all_releases->len);
  for (guint i = 0; i < self->all_releases->len; i++)
    g_ptr_array_add (ordered, g_ptr_array_index (self->all_releases, i));
  g_ptr_array_sort_with_data (ordered, compare_releases, self);

  for (guint i = 0; i < ordered->len; i++) {
    const Release *r = g_ptr_array_index (ordered, i);

    /* Heading: the release, then what kind it is and when -- the same shape as
     * the page title's name-then-year. */
    GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top (head, i == 0 ? 0 : 18);

    GtkWidget *name = gtk_label_new (r->name);
    gtk_widget_add_css_class (name, "section-heading");
    gtk_label_set_xalign (GTK_LABEL (name), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (head), name);

    g_autofree gchar *meta = r->year > 0
      ? g_strdup_printf ("%s · %d", KIND_LABELS[release_kind_of (r)], r->year)
      : g_strdup (KIND_LABELS[release_kind_of (r)]);
    GtkWidget *meta_label = gtk_label_new (meta);
    gtk_widget_add_css_class (meta_label, "dim-text");
    gtk_widget_set_valign (meta_label, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom (meta_label, 2);
    gtk_box_append (GTK_BOX (head), meta_label);

    gtk_box_append (GTK_BOX (self->releases_box), head);

    /* The release's tracks, as an ordinary list so rows behave as they do
     * everywhere else. Inline, so the page keeps doing the scrolling. */
    SpotifyGtkTrackList *list = spotifygtk_track_list_new ();
    spotifygtk_track_list_set_inline (list, TRUE);
    spotifygtk_track_list_set_numbered (list, TRUE);
    g_signal_connect (list, "track-activated",
                      G_CALLBACK (on_track_activated), self);
    if (self->wire_list)
      self->wire_list (list, self->wire_data);
    spotifygtk_track_list_set_native_tracks (list, r->tracks);
    gtk_box_append (GTK_BOX (self->releases_box), GTK_WIDGET (list));
  }

  gboolean empty = (ordered->len == 0);
  gtk_label_set_text (self->releases_status, empty ? "No releases here." : "");
  gtk_widget_set_visible (GTK_WIDGET (self->releases_status), empty);
}

/* Show the direction on the active button only, as Liked Songs does. */
static void
refresh_kind_labels (SpotifyGtkArtistPage *self)
{
  for (guint i = 0; i < N_RELEASE_KINDS; i++) {
    if (i == self->sort_kind) {
      g_autofree gchar *text = g_strdup_printf ("%s %s", KIND_LABELS[i],
                                                self->sort_desc[i] ? "\u2193" : "\u2191");
      gtk_button_set_label (GTK_BUTTON (self->kind_buttons[i]), text);
    } else {
      gtk_button_set_label (GTK_BUTTON (self->kind_buttons[i]), KIND_LABELS[i]);
    }
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->kind_buttons[i]),
                                  i == self->sort_kind);
  }
}

static void
on_kind_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  ReleaseKind kind = (ReleaseKind) GPOINTER_TO_UINT (
    g_object_get_data (G_OBJECT (button), "release-kind"));

  /* Clicking the active key reverses it, as on Liked Songs. */
  if (kind == self->sort_kind)
    self->sort_desc[kind] = !self->sort_desc[kind];
  self->sort_kind = kind;

  refresh_kind_labels (self);
  apply_release_sort (self);
}


/*
 * The discography, which is a separate load from the tracks above.
 *
 * The resolve that fills Top tracks returns the artist's most-played tracks,
 * so the releases it implies are only the ones those tracks happen to sit on,
 * each showing only the tracks that appeared. That is why an album with ten
 * tracks used to show two, and why most of a catalogue was missing: it was
 * never a limit that could be raised, it was the wrong question. This asks the
 * catalogue for the discography instead.
 */
static void
on_discography_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  ArtistLoad           *cl      = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(SpotifyGtkArtistPage) self = g_weak_ref_get (&cl->page);
  guint generation = cl->generation;
  g_weak_ref_clear (&cl->page);
  g_free (cl);

  g_autoptr(GPtrArray) releases =
    spotifygtk_native_session_load_discography_finish (session, result, &err);

  if (!self)
    return;
  if (generation != self->generation)
    return;

  finish_load_part (self);

  if (!releases) {
    if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      gtk_label_set_text (self->releases_status, "Couldn't load the releases.");
      gtk_widget_set_visible (GTK_WIDGET (self->releases_status), TRUE);
    }
    return;
  }

  g_clear_pointer (&self->all_releases, g_ptr_array_unref);
  self->all_releases = g_ptr_array_new_with_free_func (release_free);

  for (guint i = 0; i < releases->len; i++) {
    const SpotifyNativeRelease *src = g_ptr_array_index (releases, i);

    /* A release whose tracks all failed to resolve would render as a heading
     * over nothing. */
    if (!src->tracks || src->tracks->len == 0)
      continue;

    Release *r = g_new0 (Release, 1);
    r->uri        = g_strdup (src->uri);
    r->name       = g_strdup (src->name ? src->name : "Unknown release");
    r->cover_id   = g_strdup (src->cover_id);
    r->year       = src->year;
    r->type       = src->type;
    r->first_seen = i;
    r->tracks     = g_ptr_array_new_with_free_func (
      (GDestroyNotify) spotifygtk_native_track_free);

    for (guint j = 0; j < src->tracks->len; j++)
      g_ptr_array_add (r->tracks, spotifygtk_native_track_copy (
        g_ptr_array_index (src->tracks, j)));

    g_ptr_array_add (self->all_releases, r);
  }

  apply_release_sort (self);
}

static void
on_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  ArtistLoad           *cl      = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(SpotifyGtkArtistPage) self = g_weak_ref_get (&cl->page);
  guint generation = cl->generation;
  g_weak_ref_clear (&cl->page);
  g_free (cl);

  g_autoptr(GPtrArray) tracks =
    spotifygtk_native_session_load_tracks_finish (session, result, &err);

  if (!self)
    return;
  if (generation != self->generation)
    return;

  finish_load_part (self);

  if (!tracks) {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;
    g_autofree gchar *msg = g_strdup_printf ("Couldn't load: %s", err->message);
    spotifygtk_track_list_clear (self->list);
    spotifygtk_track_list_set_status (self->list, msg);
    /* A failed load must not be remembered as the current URI, or a retry via
     * re-navigation would be swallowed by the same-URI no-op. */
    g_clear_pointer (&self->current_uri, g_free);
    return;
  }

  /* The year the artist's earliest visible release carries, which is the
   * closest thing to a "since" the tracks can supply. */
  gint earliest = 0;
  for (guint i = 0; i < tracks->len; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (tracks, i);
    if (t->release_year > 0 && (earliest == 0 || t->release_year < earliest))
      earliest = t->release_year;
  }
  if (earliest > 0) {
    g_autofree gchar *year = g_strdup_printf ("%d", earliest);
    gtk_label_set_text (self->year_label, year);
  }

  /*
   * The hero waits for the artist's own portrait, asked for separately in
   * show(). A track carries its album's cover and nothing else, so the
   * resolve cannot supply one -- standing a release in for it was wrong, and
   * showed the newest single where the artist should be.
   */

  /* Top tracks: the head of the resolve, which is Spotify's own ordering for
   * an artist context and so already "most played" rather than arbitrary. */
  g_autoptr(GPtrArray) top = g_ptr_array_new ();
  for (guint i = 0; i < tracks->len && top->len < ARTIST_TOP_TRACKS; i++)
    g_ptr_array_add (top, g_ptr_array_index (tracks, i));
  spotifygtk_track_list_set_native_tracks (self->list, top);

}

void
spotifygtk_artist_page_show (SpotifyGtkArtistPage *self,
                             const gchar *artist_uri, const gchar *name)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  g_return_if_fail (artist_uri != NULL);

  if (g_strcmp0 (self->current_uri, artist_uri) == 0)
    return;

  self->generation++;
  self->pending_loads = 0;
  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);

  g_free (self->current_uri);
  self->current_uri = g_strdup (artist_uri);

  gtk_label_set_text (self->title_label, name ? name : "");
  gtk_label_set_text (self->year_label, "");
  gtk_label_set_text (self->hero_caption, "");
  gtk_picture_set_paintable (self->hero_art, NULL);

  spotifygtk_track_list_clear (self->list);
  g_clear_pointer (&self->all_releases, g_ptr_array_unref);
  apply_release_sort (self);

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    spotifygtk_track_list_set_status (self->list, "Not signed in yet.");
    set_loading (self, FALSE);
    return;
  }

  self->in_flight = g_cancellable_new ();

  spotifygtk_track_list_set_status (self->list, NULL);
  gtk_label_set_text (self->releases_status, "");
  gtk_widget_set_visible (GTK_WIDGET (self->releases_status), FALSE);
  self->pending_loads = 2;
  set_loading (self, TRUE);

  /* The portrait is its own request -- a different entity kind on the same
   * batch endpoint -- so it is asked for in parallel with the resolve rather
   * than after it. */
  GWeakRef *img_ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (img_ref, self);
  spotifygtk_native_session_get_artist_image (self->session, artist_uri,
                                              on_artist_image, img_ref);

  ArtistLoad *tracks_load = g_new0 (ArtistLoad, 1);
  g_weak_ref_init (&tracks_load->page, self);
  tracks_load->generation = self->generation;
  spotifygtk_native_session_load_tracks (self->session, artist_uri,
                                         ARTIST_PAGE_LIMIT, self->in_flight,
                                         on_tracks_loaded, tracks_load);

  /* Also in parallel: three requests of its own, and much the larger job. */
  ArtistLoad *disco_load = g_new0 (ArtistLoad, 1);
  g_weak_ref_init (&disco_load->page, self);
  disco_load->generation = self->generation;
  spotifygtk_native_session_load_discography (self->session, artist_uri,
                                              self->in_flight,
                                              on_discography_loaded, disco_load);
}

void
spotifygtk_artist_page_set_list_wire (SpotifyGtkArtistPage *self,
                                      SpotifyGtkArtistListWireFunc fn,
                                      gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  self->wire_list = fn;
  self->wire_data = user_data;
}

void
spotifygtk_artist_page_set_session (SpotifyGtkArtistPage *self,
                                    SpotifyNativeSession *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  self->generation++;
  self->pending_loads = 0;
  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  set_loading (self, FALSE);
  g_set_object (&self->session, session);
}

SpotifyGtkTrackList *
spotifygtk_artist_page_get_list (SpotifyGtkArtistPage *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self), NULL);
  return self->list;
}

void
spotifygtk_artist_page_set_playing_uri (SpotifyGtkArtistPage *self,
                                        const gchar *uri, gboolean playing)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  spotifygtk_track_list_set_playing_uri (self->list, uri, playing);
}

static void
spotifygtk_artist_page_dispose (GObject *object)
{
  SpotifyGtkArtistPage *self = SPOTIFYGTK_ARTIST_PAGE (object);

  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  g_clear_object (&self->session);
  g_clear_pointer (&self->current_uri, g_free);
  g_clear_pointer (&self->all_releases, g_ptr_array_unref);

  G_OBJECT_CLASS (spotifygtk_artist_page_parent_class)->dispose (object);
}

static void
spotifygtk_artist_page_class_init (SpotifyGtkArtistPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_artist_page_dispose;

  signals[TRACK_ACTIVATED] = g_signal_new (
    "track-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[LOADING_CHANGED] = g_signal_new (
    "loading-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

/* A section heading, matching the weight the other pages give theirs. */
static GtkWidget *
section_heading (const gchar *text)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_widget_add_css_class (label, "section-heading");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  return label;
}

static void
spotifygtk_artist_page_init (SpotifyGtkArtistPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 4);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 24);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  self->kind_label = GTK_LABEL (gtk_label_new ("Artist"));
  gtk_widget_add_css_class (GTK_WIDGET (self->kind_label), "dim-text");
  gtk_label_set_xalign (self->kind_label, 0.0);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->kind_label));

  /* Title and year share a row, the year sitting just past the end of the
   * title rather than out at the right margin -- the same arrangement, and the
   * same reasoning, as the album page. */
  GtkWidget *title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_bottom (title_row, 8);

  self->title_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->title_label), "title-text");
  gtk_label_set_xalign (self->title_label, 0.0);
  gtk_label_set_ellipsize (self->title_label, PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (title_row), GTK_WIDGET (self->title_label));

  self->year_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->year_label), "dim-text");
  gtk_label_set_xalign (self->year_label, 0.0);
  gtk_widget_set_valign (GTK_WIDGET (self->year_label), GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->year_label), 6);
  gtk_box_append (GTK_BOX (title_row), GTK_WIDGET (self->year_label));

  GtkWidget *title_slack = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (title_slack, TRUE);
  gtk_box_append (GTK_BOX (title_row), title_slack);

  self->follow_btn = gtk_button_new_with_label ("Follow");
  gtk_widget_add_css_class (self->follow_btn, "flat");
  gtk_widget_set_valign (self->follow_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->follow_btn, FALSE);
  g_signal_connect (self->follow_btn, "clicked",
                    G_CALLBACK (on_follow_clicked), self);
  gtk_box_append (GTK_BOX (title_row), self->follow_btn);

  self->title_row = title_row;
  gtk_box_append (GTK_BOX (self), title_row);

  /*
   * Everything below the title scrolls as one. The sections inside are the
   * shared list and grid in inline mode, so neither scrolls on its own -- see
   * the note on set_inline().
   */
  GtkWidget *scroller = gtk_scrolled_window_new ();
  self->scroller = scroller;
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  /*
   * The wheel animation every other page has. Its absence here is why
   * scrolling seemed to lose smoothness at random: it was not intermittent,
   * it was this page. Every release's track list is inline and therefore does
   * not scroll itself, so without this the page fell through to
   * GtkScrolledWindow's stepped wheel handling -- one notch, one jump.
   */
  spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (scroller),
                                   GTK_ORIENTATION_VERTICAL);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_end (content, CONTENT_MARGIN_END);

  /*
   * The hero: one curved panel the full width of the page, carrying the
   * artist's current media.
   *
   * Not a small portrait with text beside it. That arrangement is the shape of
   * the page this replaces, where the header is chrome and the content starts
   * underneath -- here the media is the first thing on the page and is given
   * the room to be it.
   */
  GtkWidget *hero = gtk_overlay_new ();
  gtk_widget_add_css_class (hero, "artist-hero");
  gtk_widget_set_hexpand (hero, TRUE);
  /* Covering overflows the allocation by design; clip it, and to the rounded
   * corners rather than a square. */
  gtk_widget_set_overflow (hero, GTK_OVERFLOW_HIDDEN);

  /* The only thing that gets measured. */
  GtkWidget *sizer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (sizer, -1, HERO_HEIGHT);
  gtk_overlay_set_child (GTK_OVERLAY (hero), sizer);

  /*
   * A GtkPicture, not a GtkImage. An image draws at an icon size and centres
   * what it is given, which is how the avatar ended up as a small square in
   * the middle of the banner. A picture can be told to cover its allocation,
   * which is what a header image is for -- CONTAIN would letterbox it back
   * into that same centred square.
   */
  self->hero_art = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (self->hero_art, GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink (self->hero_art, TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->hero_art), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->hero_art), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->hero_art), "artist-hero-art");
  gtk_overlay_add_overlay (GTK_OVERLAY (hero), GTK_WIDGET (self->hero_art));

  /* What the panel is showing, since it is a release standing in for imagery
   * the protocol does not carry. Sits inside the panel, under the art. */
  self->hero_caption = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->hero_caption), "dim-text");
  gtk_widget_set_halign (GTK_WIDGET (self->hero_caption), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->hero_caption), 12);
  gtk_label_set_ellipsize (self->hero_caption, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (self->hero_caption, 48);
  /* Added after the art so it sits above it. */
  gtk_widget_set_valign (GTK_WIDGET (self->hero_caption), GTK_ALIGN_END);
  gtk_overlay_add_overlay (GTK_OVERLAY (hero), GTK_WIDGET (self->hero_caption));

  gtk_box_append (GTK_BOX (content), hero);

  /* Most played. */
  /* Not "Popular" -- that is the other client's word for this, and the whole
   * point of the page is not to be a copy of it. */
  gtk_box_append (GTK_BOX (content), section_heading ("Top tracks"));

  self->list = spotifygtk_track_list_new ();
  spotifygtk_track_list_set_inline (self->list, TRUE);
  spotifygtk_track_list_set_numbered (self->list, TRUE);
  g_signal_connect (self->list, "track-activated",
                    G_CALLBACK (on_track_activated), self);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->list));

  /*
   * Releases, with the filter presented the way Liked Songs presents its sort:
   * a caption and a linked group of toggles pushed to the right of the
   * heading, so the two pages read as the same control.
   */
  GtkWidget *releases_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top (releases_row, 8);
  gtk_box_append (GTK_BOX (releases_row), section_heading ("Albums, EPs and Singles"));

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (releases_row), spacer);

  GtkWidget *caption = gtk_label_new ("Sort by");
  gtk_widget_add_css_class (caption, "dim-text");
  gtk_widget_set_valign (caption, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (releases_row), caption);

  GtkWidget *kind_group = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (kind_group, "linked");
  gtk_widget_set_valign (kind_group, GTK_ALIGN_CENTER);

  for (guint i = 0; i < N_RELEASE_KINDS; i++) {
    GtkWidget *b = gtk_toggle_button_new_with_label (KIND_LABELS[i]);
    gtk_widget_add_css_class (b, "flat");
    g_object_set_data (G_OBJECT (b), "release-kind", GUINT_TO_POINTER (i));
    g_signal_connect (b, "clicked", G_CALLBACK (on_kind_clicked), self);
    gtk_box_append (GTK_BOX (kind_group), b);
    self->kind_buttons[i] = b;
  }
  refresh_kind_labels (self);
  gtk_box_append (GTK_BOX (releases_row), kind_group);
  gtk_box_append (GTK_BOX (content), releases_row);

  self->releases_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append (GTK_BOX (content), self->releases_box);

  self->releases_status = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->releases_status), "dim-text");
  gtk_label_set_xalign (self->releases_status, 0.0);
  gtk_widget_set_visible (GTK_WIDGET (self->releases_status), FALSE);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->releases_status));

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), content);
  gtk_box_append (GTK_BOX (self), scroller);

  gtk_widget_add_tick_callback (GTK_WIDGET (self), align_title_to_hero, self, NULL);
}

SpotifyGtkArtistPage *
spotifygtk_artist_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_ARTIST_PAGE, NULL);
}

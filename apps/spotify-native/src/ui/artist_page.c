/*
 * artist_page.c — see artist_page.h.
 */

#include "artist_page.h"

#include "album_grid.h"
#include "cover_loader.h"

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

/* Height of the hero panel. Tall enough to read as a banner rather than an
 * oversized row, short enough that the tracks are still on screen with it. */
#define HERO_HEIGHT         220
#define HERO_ART_PX         180

/*
 * Release kinds.
 *
 * Inferred from how many of the artist's tracks a release contributes, because
 * nothing in the metadata says which it is: Album.type exists in Spotify's
 * schema but context-resolve returns tracks, not album objects, so it never
 * reaches us. The thresholds are the usual industry shape -- one or two tracks
 * is a single, up to six an EP -- and being a heuristic is why the filter is
 * presented as a view of one list rather than as three separate things.
 */
typedef enum {
  RELEASE_ALL = 0,
  RELEASE_ALBUM,
  RELEASE_EP,
  RELEASE_SINGLE,
  N_RELEASE_KINDS
} ReleaseKind;

static const gchar *const KIND_LABELS[N_RELEASE_KINDS] = {
  "All", "Albums", "EPs", "Singles"
};

#define EP_MAX_TRACKS      6
#define SINGLE_MAX_TRACKS  2

/* One release gathered from the artist's tracks. */
typedef struct {
  gchar *uri;
  gchar *name;
  gchar *cover_id;
  guint  track_count;
  gint   year;
  guint  first_seen;   /* keeps the resolve's own ordering as the tiebreak */
} Release;

static void
release_free (gpointer data)
{
  Release *r = data;
  g_free (r->uri);
  g_free (r->name);
  g_free (r->cover_id);
  g_free (r);
}

struct _SpotifyGtkArtistPage {
  GtkBox parent_instance;

  GtkLabel            *kind_label;
  GtkLabel            *title_label;
  GtkLabel            *year_label;

  GtkImage            *hero_art;
  GtkLabel            *hero_caption;

  SpotifyGtkTrackList *list;
  SpotifyGtkAlbumGrid *releases;
  GtkLabel            *releases_status;
  GtkWidget           *kind_buttons[N_RELEASE_KINDS];
  ReleaseKind          kind;

  GPtrArray           *all_releases;   /* Release*, in first-seen order */

  SpotifyNativeSession *session;
  GCancellable         *in_flight;
  gchar                *current_uri;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkArtistPage, spotifygtk_artist_page, GTK_TYPE_BOX)

enum { TRACK_ACTIVATED, ALBUM_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

/*
 * Written out rather than casting gtk_image_set_from_paintable to the callback
 * type: it takes (image, paintable) and the callback delivers (texture,
 * user_data), so the cast passes the texture as the image and GTK rejects it.
 */
static void
on_hero_cover_loaded (GdkTexture *texture, gpointer user_data)
{
  GtkImage *image = user_data;
  if (!texture || !GTK_IS_IMAGE (image))
    return;
  gtk_image_set_from_paintable (image, GDK_PAINTABLE (texture));
}

static void
on_track_activated (SpotifyGtkTrackList *list, gpointer track, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  (void) list;
  g_signal_emit (self, signals[TRACK_ACTIVATED], 0, track);
}

static void
on_release_activated (SpotifyGtkAlbumGrid *grid, const gchar *uri,
                      const gchar *name, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  (void) grid;
  g_signal_emit (self, signals[ALBUM_ACTIVATED], 0, uri, name);
}

static ReleaseKind
release_kind_of (const Release *r)
{
  if (r->track_count <= SINGLE_MAX_TRACKS)
    return RELEASE_SINGLE;
  if (r->track_count <= EP_MAX_TRACKS)
    return RELEASE_EP;
  return RELEASE_ALBUM;
}

/* Rebuild the cards for the current filter. */
static void
apply_release_filter (SpotifyGtkArtistPage *self)
{
  spotifygtk_album_grid_clear (self->releases);

  guint shown = 0;
  for (guint i = 0; self->all_releases && i < self->all_releases->len; i++) {
    const Release *r = g_ptr_array_index (self->all_releases, i);
    if (self->kind != RELEASE_ALL && release_kind_of (r) != self->kind)
      continue;

    g_autofree gchar *subtitle = r->year > 0
      ? g_strdup_printf ("%s · %d", KIND_LABELS[release_kind_of (r)], r->year)
      : g_strdup (KIND_LABELS[release_kind_of (r)]);

    spotifygtk_album_grid_add_card (self->releases, r->uri, r->name,
                                    subtitle, r->cover_id);
    shown++;
  }

  gboolean empty = (shown == 0);
  gtk_label_set_text (self->releases_status,
                      empty ? "Nothing of this kind here." : "");
  gtk_widget_set_visible (GTK_WIDGET (self->releases_status), empty);
}

static void
on_kind_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkArtistPage *self = user_data;
  ReleaseKind kind = (ReleaseKind) GPOINTER_TO_UINT (
    g_object_get_data (G_OBJECT (button), "release-kind"));

  self->kind = kind;
  for (guint i = 0; i < N_RELEASE_KINDS; i++)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->kind_buttons[i]),
                                  i == kind);
  apply_release_filter (self);
}

/* Gather the distinct releases present in the resolved tracks, in the order
 * the resolve returned them. */
static void
build_releases (SpotifyGtkArtistPage *self, GPtrArray *tracks)
{
  g_clear_pointer (&self->all_releases, g_ptr_array_unref);
  self->all_releases = g_ptr_array_new_with_free_func (release_free);

  g_autoptr(GHashTable) by_uri = g_hash_table_new (g_str_hash, g_str_equal);

  for (guint i = 0; i < tracks->len; i++) {
    const SpotifyNativeTrack *t = g_ptr_array_index (tracks, i);
    if (!t || !t->album_uri || !*t->album_uri)
      continue;

    Release *r = g_hash_table_lookup (by_uri, t->album_uri);
    if (!r) {
      r = g_new0 (Release, 1);
      r->uri        = g_strdup (t->album_uri);
      r->name       = g_strdup (t->album ? t->album : "Unknown release");
      r->cover_id   = g_strdup (t->cover_id);
      r->year       = t->release_year;
      r->first_seen = i;
      g_ptr_array_add (self->all_releases, r);
      g_hash_table_insert (by_uri, r->uri, r);
    }
    r->track_count++;
    if (r->year == 0 && t->release_year > 0)
      r->year = t->release_year;
  }
}

static void
on_tracks_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SpotifyNativeSession *session = SPOTIFYGTK_NATIVE_SESSION (source);
  GWeakRef             *ref     = user_data;
  g_autoptr(GError)     err     = NULL;

  g_autoptr(SpotifyGtkArtistPage) self = g_weak_ref_get (ref);
  g_weak_ref_clear (ref);
  g_free (ref);

  g_autoptr(GPtrArray) tracks =
    spotifygtk_native_session_load_tracks_finish (session, result, &err);

  if (!self)
    return;

  g_clear_object (&self->in_flight);

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

  /* The hero borrows the first track's art. Spotify has artist imagery but it
   * is not on this path -- context-resolve returns tracks, and a track carries
   * its album's cover and nothing else -- so this is the artist's most current
   * release standing in for a portrait, which is what it is captioned as. */
  const SpotifyNativeTrack *first = tracks->len > 0
    ? g_ptr_array_index (tracks, 0) : NULL;
  if (first && first->cover_id)
    spotifygtk_cover_load (first->cover_id, HERO_ART_PX, NULL,
                           on_hero_cover_loaded, self->hero_art);
  if (first)
    gtk_label_set_text (self->hero_caption,
                        first->album ? first->album : "");

  /* Top tracks: the head of the resolve, which is Spotify's own ordering for
   * an artist context and so already "most played" rather than arbitrary. */
  g_autoptr(GPtrArray) top = g_ptr_array_new ();
  for (guint i = 0; i < tracks->len && top->len < ARTIST_TOP_TRACKS; i++)
    g_ptr_array_add (top, g_ptr_array_index (tracks, i));
  spotifygtk_track_list_set_native_tracks (self->list, top);

  build_releases (self, tracks);
  apply_release_filter (self);
}

void
spotifygtk_artist_page_show (SpotifyGtkArtistPage *self,
                             const gchar *artist_uri, const gchar *name)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
  g_return_if_fail (artist_uri != NULL);

  if (g_strcmp0 (self->current_uri, artist_uri) == 0)
    return;

  g_free (self->current_uri);
  self->current_uri = g_strdup (artist_uri);

  gtk_label_set_text (self->title_label, name ? name : "");
  gtk_label_set_text (self->year_label, "");
  gtk_label_set_text (self->hero_caption, "");
  gtk_image_set_from_icon_name (self->hero_art, "avatar-default-symbolic");
  gtk_image_set_pixel_size (self->hero_art, HERO_ART_PX);

  spotifygtk_track_list_clear (self->list);
  spotifygtk_album_grid_clear (self->releases);
  g_clear_pointer (&self->all_releases, g_ptr_array_unref);

  if (!self->session ||
      spotifygtk_native_session_get_state (self->session) != SPOTIFYGTK_SESSION_READY) {
    spotifygtk_track_list_set_status (self->list, "Not signed in yet.");
    return;
  }

  if (self->in_flight)
    g_cancellable_cancel (self->in_flight);
  g_clear_object (&self->in_flight);
  self->in_flight = g_cancellable_new ();

  spotifygtk_track_list_set_status (self->list, "Loading…");

  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, self);
  spotifygtk_native_session_load_tracks (self->session, artist_uri,
                                         ARTIST_PAGE_LIMIT, self->in_flight,
                                         on_tracks_loaded, ref);
}

void
spotifygtk_artist_page_set_session (SpotifyGtkArtistPage *self,
                                    SpotifyNativeSession *session)
{
  g_return_if_fail (SPOTIFYGTK_IS_ARTIST_PAGE (self));
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
  signals[ALBUM_ACTIVATED] = g_signal_new (
    "album-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
    NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
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
  gtk_box_append (GTK_BOX (self), title_row);

  /*
   * Everything below the title scrolls as one. The sections inside are the
   * shared list and grid in inline mode, so neither scrolls on its own -- see
   * the note on set_inline().
   */
  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_end (content, 12);

  /* Hero: the artist's current release, as a banner. */
  GtkWidget *hero = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_add_css_class (hero, "artist-hero");
  gtk_widget_set_size_request (hero, -1, HERO_HEIGHT);

  self->hero_art = GTK_IMAGE (gtk_image_new_from_icon_name ("avatar-default-symbolic"));
  gtk_image_set_pixel_size (self->hero_art, HERO_ART_PX);
  gtk_widget_add_css_class (GTK_WIDGET (self->hero_art), "art-large");
  gtk_widget_set_margin_start (GTK_WIDGET (self->hero_art), 20);
  gtk_widget_set_valign (GTK_WIDGET (self->hero_art), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (hero), GTK_WIDGET (self->hero_art));

  GtkWidget *hero_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_valign (hero_text, GTK_ALIGN_CENTER);

  GtkWidget *hero_kind = gtk_label_new ("Latest release");
  gtk_widget_add_css_class (hero_kind, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (hero_kind), 0.0);
  gtk_box_append (GTK_BOX (hero_text), hero_kind);

  self->hero_caption = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->hero_caption), "section-heading");
  gtk_label_set_xalign (self->hero_caption, 0.0);
  gtk_label_set_ellipsize (self->hero_caption, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (self->hero_caption, 40);
  gtk_box_append (GTK_BOX (hero_text), GTK_WIDGET (self->hero_caption));

  gtk_box_append (GTK_BOX (hero), hero_text);
  gtk_box_append (GTK_BOX (content), hero);

  /* Most played. */
  gtk_box_append (GTK_BOX (content), section_heading ("Popular"));

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

  GtkWidget *caption = gtk_label_new ("Show");
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
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b), i == RELEASE_ALL);
    gtk_box_append (GTK_BOX (kind_group), b);
    self->kind_buttons[i] = b;
  }
  gtk_box_append (GTK_BOX (releases_row), kind_group);
  gtk_box_append (GTK_BOX (content), releases_row);

  self->releases = spotifygtk_album_grid_new_grid ();
  spotifygtk_album_grid_set_inline (self->releases, TRUE);
  spotifygtk_album_grid_set_content_margins (self->releases, 0, 0);
  g_signal_connect (self->releases, "album-activated",
                    G_CALLBACK (on_release_activated), self);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->releases));

  self->releases_status = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->releases_status), "dim-text");
  gtk_label_set_xalign (self->releases_status, 0.0);
  gtk_widget_set_visible (GTK_WIDGET (self->releases_status), FALSE);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->releases_status));

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), content);
  gtk_box_append (GTK_BOX (self), scroller);
}

SpotifyGtkArtistPage *
spotifygtk_artist_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_ARTIST_PAGE, NULL);
}

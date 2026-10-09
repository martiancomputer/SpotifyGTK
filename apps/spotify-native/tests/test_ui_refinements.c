/* Exercise the actual application CSS and widgets without starting playback,
 * authentication, or a window session. All persistent state is temporary. */
#include "../src/ui/window.c"
#include "../src/ui/artwork_viewer.h"

static void
settle (void)
{
  gint64 until = g_get_monotonic_time () + 600 * 1000;
  while (g_get_monotonic_time () < until) {
    while (g_main_context_iteration (NULL, FALSE));
    g_usleep (1000);
  }
}

static GtkWidget *
find_label (GtkWidget *root, const gchar *text)
{
  if (GTK_IS_LABEL (root) &&
      g_strcmp0 (gtk_label_get_text (GTK_LABEL (root)), text) == 0)
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *found = find_label (child, text);
    if (found) return found;
  }
  return NULL;
}

static GtkWidget *
find_class (GtkWidget *root, const gchar *name)
{
  if (gtk_widget_has_css_class (root, name)) return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *found = find_class (child, name);
    if (found) return found;
  }
  return NULL;
}

static GtkWidget *
find_type (GtkWidget *root, GType type)
{
  if (g_type_is_a (G_OBJECT_TYPE (root), type)) return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child;
       child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *found = find_type (child, type);
    if (found) return found;
  }
  return NULL;
}

static GtkWidget *
button_for (GtkWidget *root, const gchar *text)
{
  GtkWidget *label = find_label (root, text);
  g_assert_nonnull (label);
  GtkWidget *button = gtk_widget_get_ancestor (label, GTK_TYPE_BUTTON);
  g_assert_nonnull (button);
  return button;
}

static graphene_rect_t
bounds (GtkWidget *child, GtkWidget *root)
{
  graphene_rect_t rect;
  g_assert_true (gtk_widget_compute_bounds (child, root, &rect));
  return rect;
}

static GdkTexture *
render (GtkWidget *window)
{
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new (window);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot),
    gtk_widget_get_width (window), gtk_widget_get_height (window));
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  return gsk_renderer_render_texture (
    gtk_native_get_renderer (GTK_NATIVE (window)), node, NULL);
}

static void
save_preview (GtkWidget *window, const gchar *filename)
{
  const gchar *dir = g_getenv ("SPOTIFYGTK_TEST_PREVIEWS");
  if (!dir || !*dir) return;
  g_autoptr(GdkTexture) texture = render (window);
  g_autofree gchar *path = g_build_filename (dir, filename, NULL);
  g_assert_true (gdk_texture_save_to_png (texture, path));
}

static void
test_library_local_empty_and_create_position (void)
{
  spotifygtk_settings_set_show_playlists_separately (
    spotifygtk_settings_get_default (), FALSE);
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 1280, 780);
  SpotifyGtkLibraryPage *page = spotifygtk_library_page_new ();
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (page));
  gtk_window_present (GTK_WINDOW (window));
  settle ();
  GtkWidget *entry = find_type (GTK_WIDGET (page), GTK_TYPE_SEARCH_ENTRY);
  GtkWidget *add = find_class (GTK_WIDGET (page), "playlist-create-button");
  g_assert_true (gtk_widget_get_parent (add) == gtk_widget_get_parent (entry));
  graphene_rect_t e = bounds (entry, window), a = bounds (add, window);
  g_assert_cmpfloat (a.origin.x, >, e.origin.x + e.size.width);
  g_assert_cmpfloat (a.origin.x - e.origin.x - e.size.width, <=, 12);
  g_assert_cmpfloat (fabs ((a.origin.y + a.size.height / 2) -
                          (e.origin.y + e.size.height / 2)), <=, 1);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (
    button_for (GTK_WIDGET (page), "Local Files")), TRUE);
  settle ();
  GtkWidget *stack = find_type (GTK_WIDGET (page), GTK_TYPE_STACK);
  GtkWidget *current = gtk_stack_get_visible_child (GTK_STACK (stack));
  GtkWidget *state = find_class (current, "local-files-empty-state");
  g_assert_true (gtk_widget_get_mapped (state));
  GtkWidget *vinyl = find_type (state, GTK_TYPE_IMAGE);
  g_assert_cmpstr (gtk_image_get_icon_name (GTK_IMAGE (vinyl)), ==,
                   "spotifygtk-vinyl-record");
  g_assert_true (gtk_icon_theme_has_icon (
    gtk_icon_theme_get_for_display (gdk_display_get_default ()),
    "spotifygtk-vinyl-record"));
  g_assert_cmpint (gtk_widget_get_width (vinyl), ==, 228);
  g_assert_cmpint (gtk_widget_get_height (vinyl), ==, 228);
  g_assert_nonnull (find_label (state,
    "Enable Local files and add a folder to get started."));
  save_preview (window, "local-files-dark-plus.png");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (
    button_for (GTK_WIDGET (page), "Albums")), TRUE);
  settle ();
  current = gtk_stack_get_visible_child (GTK_STACK (stack));
  state = find_class (current, "local-files-empty-state");
  g_assert_false (gtk_widget_get_visible (state));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (
    button_for (GTK_WIDGET (page), "Local Files")), TRUE);
  settle ();
  g_assert_true (gtk_widget_get_mapped (find_class (
    gtk_stack_get_visible_child (GTK_STACK (stack)), "local-files-empty-state")));
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_liked_sort_alignment (void)
{
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 1200, 600);
  SpotifyGtkLikedSongsPage *page = spotifygtk_liked_songs_page_new ();
  SpotifyGtkTrackList *list = spotifygtk_liked_songs_page_get_list (page);
  g_autoptr(GPtrArray) tracks = g_ptr_array_new ();
  SpotifyNativeTrack track = { .uri = "local:track:fixture", .name = "Track fixture",
    .artists = "Artist", .album = "Album", .duration_ms = 208000 };
  g_ptr_array_add (tracks, &track);
  spotifygtk_track_list_set_native_tracks (list, tracks);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (page));
  gtk_window_present (GTK_WINDOW (window));
  settle ();
  GtkWidget *sort = gtk_widget_get_parent (button_for (GTK_WIDGET (page), "A–Z"));
  for (guint i = 0; i < 2; i++) {
    gdouble inset;
    g_assert_true (spotifygtk_track_list_duration_inset (list, &inset));
    graphene_rect_t s = bounds (sort, GTK_WIDGET (page));
    graphene_rect_t l = bounds (GTK_WIDGET (list), GTK_WIDGET (page));
    g_assert_cmpfloat (fabs (s.origin.x + s.size.width -
                            (l.origin.x + l.size.width - inset)), <=, 1);
    gtk_window_set_default_size (GTK_WINDOW (window), 900, 600);
    settle ();
  }
  save_preview (window, "liked-sort-aligned.png");
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_right_collapse_style (void)
{
  g_autoptr(SpotifyGtkNowPlayingPanel) panel =
    g_object_ref_sink (spotifygtk_now_playing_panel_new ());
  GtkWidget *button = button_for (GTK_WIDGET (panel), "Collapse");
  GtkWidget *label = find_label (button, "Collapse");
  g_assert_true (gtk_widget_has_css_class (button, "flat"));
  g_assert_true (gtk_widget_has_css_class (label, "sidebar-action"));
  GtkWidget *icon = gtk_widget_get_first_child (gtk_button_get_child (GTK_BUTTON (button)));
  g_assert_true (GTK_IS_IMAGE (icon));
  g_assert_cmpstr (gtk_image_get_icon_name (GTK_IMAGE (icon)), ==, "go-next-symbolic");
  g_assert_cmpint (gtk_image_get_pixel_size (GTK_IMAGE (icon)), ==, 14);
}

static void
assert_pixel (GdkTexture *texture, gint x, gint y, guint8 gray)
{
  gint stride = gdk_texture_get_width (texture) * 4;
  g_autofree guchar *data = g_malloc (stride * gdk_texture_get_height (texture));
  gdk_texture_download (texture, data, stride);
  const guchar *pixel = data + y * stride + x * 4;
  for (guint i = 0; i < 3; i++)
    g_assert_cmpint (ABS (pixel[i] - gray), <=, 1);
}

static void
test_dialog_palette (void)
{
  const SpotifyGtkTheme themes[] = { SPOTIFYGTK_THEME_DARK_PLUS,
    SPOTIFYGTK_THEME_DARK, SPOTIFYGTK_THEME_LIGHT };
  const guint8 surfaces[] = { 0x12, 0x1a, 0xee };
  const guint8 controls[] = { 0x20, 0x1f, 0xe4 };
  GtkWidget *window = adw_application_window_new (NULL);
  gtk_window_set_default_size (GTK_WINDOW (window), 1000, 700);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window),
    GTK_WIDGET (spotifygtk_library_page_new ()));
  gtk_window_present (GTK_WINDOW (window));
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
    adw_alert_dialog_new ("Rename playlist", "The new name will sync to Spotify."));
  spotifygtk_dialog_prepare (ADW_DIALOG (dialog));
  GtkWidget *entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry), "Playlist fixture");
  adw_alert_dialog_set_extra_child (dialog, entry);
  adw_alert_dialog_add_responses (dialog, "cancel", "Cancel", "rename", "Rename", NULL);
  adw_dialog_present (ADW_DIALOG (dialog), window);
  for (guint i = 0; i < G_N_ELEMENTS (themes); i++) {
    apply_theme (themes[i]);
    settle ();
    graphene_rect_t e = bounds (entry, window);
    GtkWidget *cancel = button_for (GTK_WIDGET (dialog), "Cancel");
    graphene_rect_t c = bounds (cancel, window);
    g_autoptr(GdkTexture) texture = render (window);
    assert_pixel (texture, (gint) e.origin.x - 12,
                  (gint) (e.origin.y + e.size.height / 2), surfaces[i]);
    assert_pixel (texture, (gint) (c.origin.x + 20),
                  (gint) (c.origin.y + c.size.height / 2), controls[i]);
    const gchar *previews[] = { "rename-dark-plus.png", "rename-dark.png", "rename-light.png" };
    save_preview (window, previews[i]);
  }
  adw_dialog_close (ADW_DIALOG (dialog));
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_artwork_overlay (void)
{
  GtkWidget *window = adw_application_window_new (NULL);
  spotifygtk_dialog_host_bind (ADW_APPLICATION_WINDOW (window));
  gtk_window_set_default_size (GTK_WINDOW (window), 1200, 950);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window), gtk_label_new ("Background"));
  gtk_window_present (GTK_WINDOW (window));
  settle ();
  /* A missing local image never makes a network request. Install a known
   * full-size texture to inspect the actual application dialog CSS. */
  spotifygtk_artwork_viewer_present (window, "local:missing-fixture", "Artwork fixture");
  AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window));
  g_assert_nonnull (dialog);
  g_object_ref (dialog);
  GtkPicture *picture = GTK_PICTURE (find_type (GTK_WIDGET (dialog), GTK_TYPE_PICTURE));
  g_assert_nonnull (picture);
  g_autofree guint8 *pixels = g_malloc (640 * 640 * 4);
  memset (pixels, 0xff, 640 * 640 * 4);
  g_autoptr(GBytes) bytes = g_bytes_new (pixels, 640 * 640 * 4);
  g_autoptr(GdkTexture) artwork = gdk_memory_texture_new (640, 640, GDK_MEMORY_R8G8B8A8, bytes, 640 * 4);
  gtk_picture_set_paintable (picture, GDK_PAINTABLE (artwork));
  GtkWidget *status = find_label (GTK_WIDGET (dialog), "Artwork could not be loaded.\nClick outside or press Escape to close.");
  g_assert_nonnull (status);
  gtk_widget_set_visible (status, FALSE);
  adw_dialog_set_content_width (dialog, 640);
  adw_dialog_set_content_height (dialog, 640);
  const SpotifyGtkTheme themes[] = { SPOTIFYGTK_THEME_LIGHT, SPOTIFYGTK_THEME_DARK_PLUS };
  for (guint i = 0; i < G_N_ELEMENTS (themes); i++) {
    apply_theme (themes[i]); settle ();
    graphene_rect_t rect = bounds (GTK_WIDGET (picture), window);
    g_assert_cmpfloat (fabs (rect.origin.x + rect.size.width / 2 - gtk_widget_get_width (window) / 2.0), <=, 1);
    g_assert_cmpfloat (fabs (rect.origin.y + rect.size.height / 2 - gtk_widget_get_height (window) / 2.0), <=, 1);
    g_autoptr(GdkTexture) snapshot = render (window);
    save_preview (window, i ? "artwork-overlay-dark-plus.png" : "artwork-overlay-light.png");
    assert_pixel (snapshot, rect.origin.x + 2, rect.origin.y + 2, 255);
    gint stride = gdk_texture_get_width (snapshot) * 4;
    g_autofree guint8 *image = g_malloc (stride * gdk_texture_get_height (snapshot));
    gdk_texture_download (snapshot, image, stride);
    gint y = rect.origin.y + rect.size.height / 2;
    /* No frame or shadow between art and backdrop. Average a short strip
     * because GL dithering varies individual near-black pixels. */
    for (guint c = 0; c < 3; c++) {
      gint near = 0, far = 0;
      for (gint dy = 0; dy < 16; dy++) {
        near += image[(y + dy) * stride + ((gint) rect.origin.x - 2) * 4 + c];
        far += image[(y + dy) * stride + ((gint) rect.origin.x - 40) * 4 + c];
      }
      g_assert_cmpint (ABS (near - far), <=, 16);
    }
  }
  adw_dialog_close (dialog);
  g_assert_null (gtk_picture_get_paintable (picture));
  settle ();
  g_object_unref (dialog);
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_home_shelves (void)
{
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 1200, 700);
  SpotifyGtkHomePage *page = spotifygtk_home_page_new ();
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (page));
  SpotifyHomeCard a = { "spotify:album:0000000000000000000000", "Album fixture", "Artist", "" };
  SpotifyHomeCard p = { "spotify:playlist:0000000000000000000000", "Playlist fixture", "Playlist", "" };
  g_autoptr(GPtrArray) cards = g_ptr_array_new ();
  g_ptr_array_add (cards, &a); g_ptr_array_add (cards, &p);
  SpotifyHomeSection first = { "Continue listening", "", cards };
  SpotifyHomeSection second = { "Recently played", "Fixture subtitle", cards };
  g_autoptr(GPtrArray) sections = g_ptr_array_new ();
  g_ptr_array_add (sections, &first); g_ptr_array_add (sections, &second);
  SpotifyHomeFeed feed = { "Hello fixture", sections };
  spotifygtk_home_page_set_feed (page, &feed);
  gtk_window_present (GTK_WINDOW (window));
  settle ();
  g_assert_cmpuint (spotifygtk_home_page_get_grids (page)->len, ==, 2);
  GtkWidget *heading = find_label (GTK_WIDGET (page), "Jump back in");
  GtkWidget *recent = find_label (GTK_WIDGET (page), "For you");
  g_assert_cmpfloat (bounds (recent, window).origin.y, >, bounds (heading, window).origin.y);
  SpotifyGtkAlbumGrid *grid = g_ptr_array_index (spotifygtk_home_page_get_grids (page), 0);
  SpotifyGtkAlbumGrid *retained = g_ptr_array_index (spotifygtk_home_page_get_grids (page), 1);
  GtkListView *view = GTK_LIST_VIEW (gtk_scrolled_window_get_child (
    spotifygtk_album_grid_get_scroller (retained)));
  g_assert_nonnull (gtk_list_view_get_model (view));
  g_autoptr(GObject) before = g_list_model_get_item (
    G_LIST_MODEL (gtk_list_view_get_model (view)), 0);
  /* A changed greeting must not destroy unchanged card models or artwork. */
  feed.greeting = "Updated greeting";
  spotifygtk_home_page_set_feed (page, &feed);
  settle ();
  g_autoptr(GObject) after = g_list_model_get_item (
    G_LIST_MODEL (gtk_list_view_get_model (view)), 0);
  g_assert_true (before == after);
  g_assert_true (grid == g_ptr_array_index (spotifygtk_home_page_get_grids (page), 0));
  g_ptr_array_set_size (sections, 1);
  spotifygtk_home_page_set_feed (page, &feed);
  g_assert_nonnull (find_label (GTK_WIDGET (page), "For you"));
  g_ptr_array_add (sections, &second);
  spotifygtk_home_page_set_feed (page, &feed);
  settle ();
  g_assert_nonnull (gtk_list_view_get_model (view));
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (gtk_list_view_get_model (view))), ==, 2);
  spotifygtk_home_page_set_session (page, NULL);
  g_assert_true (gtk_widget_get_visible (gtk_widget_get_parent (heading)));
  spotifygtk_home_page_clear_cache (page);
  g_assert_false (gtk_widget_get_mapped (heading));
  g_assert_nonnull (find_label (GTK_WIDGET (page),
    "Sign in to see your personalised Home. Local files are available in Library."));
  gtk_window_destroy (GTK_WINDOW (window));
}

static guint
widget_count (GtkWidget *widget)
{
  guint n = 1;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child)) n += widget_count (child);
  return n;
}

static void
test_home_large_feed (void)
{
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 1200, 700);
  SpotifyGtkHomePage *page = spotifygtk_home_page_new ();
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (page));
  SpotifyHomeCard card = { "spotify:album:0000000000000000000000", "Fixture album", "Fixture artist", "" };
  g_autoptr(GPtrArray) cards = g_ptr_array_new ();
  for (guint i = 0; i < 10; i++) g_ptr_array_add (cards, &card);
  SpotifyHomeSection section = { "Fixture shelf", "", cards };
  g_autoptr(GPtrArray) sections = g_ptr_array_new ();
  for (guint i = 0; i < 20; i++) g_ptr_array_add (sections, &section);
  SpotifyHomeFeed feed = { "Fixture greeting", sections };
  spotifygtk_home_page_set_feed (page, &feed);
  gtk_window_present (GTK_WINDOW (window));
  settle ();
  g_test_message ("20-shelf Home widget count: %u", widget_count (GTK_WIDGET (page)));
  /* Same card-row budget as before, plus 80 widgets for the 20 pairs of
   * icon buttons. The navigation controls exist even on detached shelves. */
  g_assert_cmpuint (widget_count (GTK_WIDGET (page)), <, 880);
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (
    find_type (GTK_WIDGET (page), GTK_TYPE_SCROLLED_WINDOW));
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (scroller);
  GtkWidget *viewport = gtk_scrolled_window_get_child (scroller);
  GtkWidget *content = gtk_viewport_get_child (GTK_VIEWPORT (viewport));
  graphene_rect_t content_rect = bounds (content, GTK_WIDGET (scroller));
  graphene_rect_t scrollbar_rect = bounds (gtk_scrolled_window_get_vscrollbar (scroller),
                                           GTK_WIDGET (scroller));
  g_assert_cmpfloat_with_epsilon (content_rect.origin.x, 34, 0.5);
  g_assert_cmpfloat_with_epsilon (scrollbar_rect.origin.x -
    (content_rect.origin.x + content_rect.size.width), 2, 0.5);
  g_assert_cmpfloat_with_epsilon (scrollbar_rect.origin.x + scrollbar_rect.size.width,
    gtk_widget_get_width (GTK_WIDGET (scroller)), 0.5);
  gdouble upper = gtk_adjustment_get_upper (adjustment);
  gtk_adjustment_set_value (adjustment, 2800);
  settle ();
  /* While vertical events continue faster than the old 180ms settle delay,
   * shelves in the viewport must already have their rows, not blank space. */
  for (guint frame = 0; frame < 20; frame++) {
    gtk_adjustment_set_value (adjustment, 2800 + frame * 30);
    gint64 until = g_get_monotonic_time () + 40 * 1000;
    while (g_get_monotonic_time () < until) {
      while (g_main_context_iteration (NULL, FALSE));
      g_usleep (1000);
    }
    if (frame < 3) continue;
    guint visible = 0;
    GPtrArray *grids = spotifygtk_home_page_get_grids (page);
    for (guint i = 0; i < grids->len; i++) {
      SpotifyGtkAlbumGrid *grid = g_ptr_array_index (grids, i);
      if (!gtk_widget_get_mapped (GTK_WIDGET (grid))) continue;
      graphene_rect_t rect = bounds (GTK_WIDGET (grid), GTK_WIDGET (scroller));
      if (rect.origin.y < gtk_widget_get_height (GTK_WIDGET (scroller)) &&
          rect.origin.y + rect.size.height > 0) {
        GtkListView *view = GTK_LIST_VIEW (gtk_scrolled_window_get_child (
          spotifygtk_album_grid_get_scroller (grid)));
        g_assert_nonnull (gtk_list_view_get_model (view));
        visible++;
      }
    }
    g_assert_cmpuint (visible, >, 0);
  }
  settle ();
  g_assert_cmpfloat (fabs (gtk_adjustment_get_upper (adjustment) - upper), <=, 1);
  g_assert_cmpuint (widget_count (GTK_WIDGET (page)), <, 880);
  gdouble position = gtk_adjustment_get_value (adjustment);
  spotifygtk_home_page_set_covers_loaded (page, FALSE);
  settle ();
  g_test_message ("Inactive Home widget count: %u", widget_count (GTK_WIDGET (page)));
  /* Includes the persistent two-button navigation controls on 20 headings;
   * inactive shelves must still release their much larger card row trees. */
  g_assert_cmpuint (widget_count (GTK_WIDGET (page)), <, 580);
  spotifygtk_home_page_set_covers_loaded (page, TRUE);
  settle ();
  g_assert_cmpfloat (fabs (gtk_adjustment_get_value (adjustment) - position), <=, 1);
  g_assert_cmpuint (widget_count (GTK_WIDGET (page)), <, 880);
  gtk_window_destroy (GTK_WINDOW (window));
}

typedef struct { gchar *uri; guint contexts; gboolean play; gchar *destination; } HomeAction;

static void
capture_home_context (SpotifyGtkHomePage *page, const gchar *uri, const gchar *title,
                      gboolean play, gpointer data)
{
  HomeAction *action = data;
  g_free (action->uri); action->uri = g_strdup (uri);
  action->play = play; action->contexts++;
  (void) page; (void) title;
}

static void
capture_home_destination (SpotifyGtkHomePage *page, const gchar *destination, gpointer data)
{
  HomeAction *action = data;
  g_free (action->destination);
  action->destination = g_strdup (destination);
  (void) page;
}

/* Sample empty areas, averaging GL's near-black dithering. Nested row/button
 * fills used to produce visibly different rectangles within one Home item. */
static void
assert_same_surface (GdkTexture *texture, gint ax, gint ay, gint bx, gint by)
{
  gint stride = gdk_texture_get_width (texture) * 4;
  g_autofree guint8 *pixels = g_malloc (stride * gdk_texture_get_height (texture));
  gdk_texture_download (texture, pixels, stride);
  for (guint c = 0; c < 3; c++) {
    guint a = 0, b = 0;
    for (guint i = 0; i < 4; i++) {
      a += pixels[(ay + i) * stride + ax * 4 + c];
      b += pixels[(by + i) * stride + bx * 4 + c];
    }
    g_assert_cmpint (ABS ((gint) a - (gint) b), <=, 4);
  }
}

static void
assert_card_text_alignment (GtkWidget *card, GtkWidget *window)
{
  GtkWidget *art = g_object_get_data (G_OBJECT (card), "art");
  GtkWidget *title = g_object_get_data (G_OBJECT (card), "title");
  GtkWidget *sub = g_object_get_data (G_OBJECT (card), "sub");
  graphene_rect_t a = bounds (art, window);
  graphene_rect_t t = bounds (title, window);
  graphene_rect_t s = bounds (sub, window);
  g_assert_cmpfloat_with_epsilon (t.origin.x, a.origin.x, 0.5);
  g_assert_cmpfloat_with_epsilon (s.origin.x, a.origin.x, 0.5);
  g_assert_cmpfloat_with_epsilon (t.size.width, a.size.width, 0.5);
  g_assert_cmpfloat_with_epsilon (s.size.width, a.size.width, 0.5);
}

static void
test_shared_card_design (void)
{
  const SpotifyGtkTheme themes[] = { SPOTIFYGTK_THEME_LIGHT,
    SPOTIFYGTK_THEME_DARK, SPOTIFYGTK_THEME_DARK_PLUS };
  const gchar *previews[2][3] = {
    { "library-cards-light.png", "library-cards-dark.png", "library-cards-dark-plus.png" },
    { "search-cards-light.png", "search-cards-dark.png", "search-cards-dark-plus.png" }
  };
  /* The Library grid and Search's viewport-sized shelf use the same factory.
   * Exercise both, without changing their scrollbar or sizing policies. */
  for (guint shelf = 0; shelf < 2; shelf++) {
    GtkWidget *window = gtk_window_new ();
    gtk_window_set_default_size (GTK_WINDOW (window), 1000, 650);
    SpotifyGtkAlbumGrid *grid = shelf ? spotifygtk_album_grid_new_shelf ()
                                      : spotifygtk_album_grid_new_grid ();
    if (shelf) spotifygtk_album_grid_set_full_card_shelf (grid, TRUE);
    gtk_widget_set_margin_start (GTK_WIDGET (grid), 20);
    gtk_widget_set_margin_end (GTK_WIDGET (grid), 20);
    gtk_widget_set_margin_top (GTK_WIDGET (grid), 20);
    gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (grid));
    SpotifyGtkCardSpec cards[] = {
      { .uri = "spotify:album:0000000000000000000001", .title = "Long album fixture title that must stay within its artwork", .subtitle = "Long artist fixture name that must also stay within its artwork" },
      { .uri = "spotify:playlist:0000000000000000000002", .title = "Playlist fixture", .subtitle = "Playlist" },
      { .uri = "local:album:fixture", .title = "Local album", .subtitle = "2026" },
      { .uri = "spotify:artist:0000000000000000000003", .title = "Artist fixture", .subtitle = "Followed artist" }
    };
    spotifygtk_album_grid_set_cards (grid, cards, G_N_ELEMENTS (cards));
    gtk_window_present (GTK_WINDOW (window)); settle ();
    gtk_window_set_focus (GTK_WINDOW (window), NULL);
    GtkPolicyType horizontal, vertical;
    gtk_scrolled_window_get_policy (spotifygtk_album_grid_get_scroller (grid), &horizontal, &vertical);
    g_assert_cmpint (horizontal, ==, shelf ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER);
    g_assert_cmpint (vertical, ==, shelf ? GTK_POLICY_NEVER : GTK_POLICY_AUTOMATIC);
    GtkWidget *card = find_class (GTK_WIDGET (grid), "media-card");
    g_assert_nonnull (card);
    GtkWidget *row = gtk_widget_get_parent (card);
    GtkWidget *art = g_object_get_data (G_OBJECT (card), "art");
    g_assert_cmpint (gtk_widget_get_overflow (art), ==, GTK_OVERFLOW_HIDDEN);
    /* Opaque coloured pixels reveal whether the real image is corner-clipped,
     * rather than just giving its empty placeholder a rounded background. */
    g_autofree guint8 *pixels = g_malloc (176 * 176 * 4);
    for (guint i = 0; i < 176 * 176; i++) {
      pixels[i * 4] = 255; pixels[i * 4 + 1] = 0;
      pixels[i * 4 + 2] = 255; pixels[i * 4 + 3] = 255;
    }
    g_autoptr(GBytes) bytes = g_bytes_new (pixels, 176 * 176 * 4);
    g_autoptr(GdkTexture) fixture = gdk_memory_texture_new (176, 176,
      GDK_MEMORY_R8G8B8A8, bytes, 176 * 4);
    gtk_image_set_from_paintable (GTK_IMAGE (art), GDK_PAINTABLE (fixture));
    for (guint i = 0; i < G_N_ELEMENTS (themes); i++) {
      apply_theme (themes[i]);
      gtk_widget_unset_state_flags (card, GTK_STATE_FLAG_PRELIGHT);
      gtk_widget_unset_state_flags (row, GTK_STATE_FLAG_PRELIGHT);
      settle ();
      graphene_rect_t c = bounds (card, window), r = bounds (row, window), a = bounds (art, window);
      assert_card_text_alignment (card, window);
      g_autoptr(GdkTexture) idle = render (window);
      g_test_message ("%s cards, theme %u: idle surface", shelf ? "Search" : "Library", i);
      save_preview (window, previews[shelf][i]);
      assert_same_surface (idle, c.origin.x + 8, c.origin.y + c.size.height - 8,
                           10, c.origin.y + c.size.height - 8);
      gint stride = gdk_texture_get_width (idle) * 4;
      g_autofree guint8 *image = g_malloc (stride * gdk_texture_get_height (idle));
      gdk_texture_download (idle, image, stride);
      guint8 *corner = image + (gint) a.origin.y * stride + (gint) a.origin.x * 4;
      guint8 *center = image + (gint) (a.origin.y + a.size.height / 2) * stride +
                       (gint) (a.origin.x + a.size.width / 2) * 4;
      gint difference = 0;
      for (guint channel = 0; channel < 3; channel++) difference += ABS (corner[channel] - center[channel]);
      g_assert_cmpint (difference, >, 10);
      gtk_widget_set_state_flags (row, GTK_STATE_FLAG_PRELIGHT, FALSE);
      gtk_widget_set_state_flags (card, GTK_STATE_FLAG_PRELIGHT, FALSE);
      settle ();
      g_autoptr(GdkTexture) hover = render (window);
      g_test_message ("%s cards, theme %u: row surface", shelf ? "Search" : "Library", i);
      if (r.origin.x + 1 < c.origin.x)
        assert_same_surface (hover, r.origin.x + 1, c.origin.y + c.size.height - 8,
                             10, c.origin.y + c.size.height - 8);
      else
        /* Search's row has no outer padding: its bounds equal the card, so
         * compare the card's two empty insets rather than an interior pixel
         * against the unhovered page background. */
        assert_same_surface (hover, c.origin.x + 8, c.origin.y + c.size.height - 8,
                             c.origin.x + c.size.width - 8, c.origin.y + c.size.height - 8);
    }
    gtk_window_set_default_size (GTK_WINDOW (window), 640, 650);
    settle ();
    assert_card_text_alignment (card, window);
    gtk_window_destroy (GTK_WINDOW (window));
  }
}

static void
assert_home_surfaces (GtkWidget *window, GtkWidget *open, SpotifyGtkAlbumGrid *grid)
{
  GtkWidget *tile = gtk_widget_get_parent (open);
  GtkWidget *play = gtk_widget_get_last_child (tile);
  GtkWidget *card = find_class (GTK_WIDGET (grid), "media-card");
  g_assert_nonnull (card);
  assert_card_text_alignment (card, window);
  GtkWidget *row = gtk_widget_get_parent (card);
  graphene_rect_t t = bounds (tile, window), o = bounds (open, window);
  graphene_rect_t p = bounds (play, window), c = bounds (card, window);
  gint y = t.origin.y + t.size.height - 8;
  g_autoptr(GdkTexture) idle = render (window);
  assert_same_surface (idle, t.origin.x + 100, y, 10, y);
  assert_same_surface (idle, c.origin.x + 8, c.origin.y + c.size.height - 8,
                       10, c.origin.y + c.size.height - 8);
  gtk_widget_set_state_flags (tile, GTK_STATE_FLAG_PRELIGHT, FALSE);
  gtk_widget_set_state_flags (open, GTK_STATE_FLAG_PRELIGHT, FALSE);
  gtk_widget_set_state_flags (play, GTK_STATE_FLAG_PRELIGHT, FALSE);
  gtk_widget_set_state_flags (row, GTK_STATE_FLAG_PRELIGHT, FALSE);
  gtk_widget_set_state_flags (card, GTK_STATE_FLAG_PRELIGHT, FALSE);
  settle ();
  g_autoptr(GdkTexture) hover = render (window);
  assert_same_surface (hover, t.origin.x + 100, y, o.origin.x + o.size.width - 8, y);
  assert_same_surface (hover, t.origin.x + 100, y, p.origin.x + p.size.width / 2, y);
  assert_same_surface (hover, t.origin.x + 100, y,
                       p.origin.x + 4, p.origin.y + p.size.height / 2);
  /* The outer ListView row stays transparent even while its card is hovered. */
  assert_same_surface (hover, c.origin.x + c.size.width + 4,
                       c.origin.y + c.size.height - 8, 10, c.origin.y + c.size.height - 8);
  gtk_widget_unset_state_flags (tile, GTK_STATE_FLAG_PRELIGHT);
  gtk_widget_unset_state_flags (open, GTK_STATE_FLAG_PRELIGHT);
  gtk_widget_unset_state_flags (play, GTK_STATE_FLAG_PRELIGHT);
  gtk_widget_unset_state_flags (row, GTK_STATE_FLAG_PRELIGHT);
  gtk_widget_unset_state_flags (card, GTK_STATE_FLAG_PRELIGHT);
  settle ();
}

static void
test_home_dashboard (void)
{
  GtkWidget *window = adw_application_window_new (NULL);
  gtk_window_set_default_size (GTK_WINDOW (window), 1070, 850);
  SpotifyGtkHomePage *page = spotifygtk_home_page_new ();
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window), GTK_WIDGET (page));
  HomeAction action = {0};
  g_signal_connect (page, "context-requested", G_CALLBACK (capture_home_context), &action);
  g_signal_connect (page, "destination-requested", G_CALLBACK (capture_home_destination), &action);
  SpotifyHomeCard fixtures[] = {
    { "spotify:album:0000000000000000000001", "Secret Level", "No Mana", "" },
    { "spotify:album:0000000000000000000002", "DRAIN ME", "Das Mörtal", "" },
    { "spotify:playlist:0000000000000000000003", "Playlist #6", "Your playlist", "" },
    { "spotify:album:0000000000000000000004", "We Are Friends, Vol. 10", "Various Artists", "" },
    { "spotify:album:0000000000000000000005", "Gave U My Love", "Tomas Heredia", "" },
    { "spotify:playlist:0000000000000000000006", "On Repeat", "Playlist", "" }
  };
  g_autoptr(GPtrArray) cards = g_ptr_array_new ();
  for (guint i = 0; i < G_N_ELEMENTS (fixtures); i++) g_ptr_array_add (cards, &fixtures[i]);
  SpotifyHomeSection recent = { "Continue listening", "", cards };
  SpotifyHomeSection fresh = { "New releases for you", "", cards };
  g_autoptr(GPtrArray) sections = g_ptr_array_new ();
  g_ptr_array_add (sections, &recent); g_ptr_array_add (sections, &fresh);
  SpotifyHomeFeed feed = { "Good afternoon", sections };
  spotifygtk_home_page_set_feed (page, &feed);
  gtk_window_present (GTK_WINDOW (window));
  settle ();
  g_assert_null (find_label (GTK_WIDGET (page), "Customize Home"));
  g_assert_nonnull (find_label (GTK_WIDGET (page), "Your music, right where you left it."));
  GtkWidget *jump = find_label (GTK_WIDGET (page), "Jump back in");
  GtkWidget *primary = find_label (GTK_WIDGET (page), "Fresh for you");
  GtkWidget *rotation = find_label (GTK_WIDGET (page), "Your rotation");
  g_assert_cmpfloat (bounds (primary, window).origin.y, >, bounds (jump, window).origin.y);
  g_assert_cmpfloat (bounds (rotation, window).origin.y, >, bounds (primary, window).origin.y);
  GtkWidget *first = button_for (GTK_WIDGET (page), "Secret Level");
  GtkWidget *third = button_for (GTK_WIDGET (page), "Playlist #6");
  GtkWidget *fourth = button_for (GTK_WIDGET (page), "We Are Friends, Vol. 10");
  g_assert_cmpfloat (fabs (bounds (third, window).origin.y - bounds (first, window).origin.y), <, 1);
  g_assert_cmpfloat (bounds (fourth, window).origin.y, >, bounds (first, window).origin.y);
  GtkWidget *rotation_first = button_for (GTK_WIDGET (page), "On repeat");
  GtkWidget *rotation_last = button_for (GTK_WIDGET (page), "Recently liked");
  g_assert_cmpfloat (fabs (bounds (rotation_first, window).origin.y -
                          bounds (rotation_last, window).origin.y), <, 1);
  g_signal_emit_by_name (first, "clicked");
  g_assert_cmpstr (action.uri, ==, fixtures[0].uri);
  g_assert_false (action.play);
  g_signal_emit_by_name (gtk_widget_get_last_child (gtk_widget_get_parent (first)), "clicked");
  g_assert_true (action.play);
  g_assert_cmpuint (action.contexts, ==, 2);
  g_signal_emit_by_name (button_for (GTK_WIDGET (page), "Local collection"), "clicked");
  g_assert_cmpstr (action.destination, ==, "local");
  g_signal_emit_by_name (button_for (GTK_WIDGET (page), "Recently liked"), "clicked");
  g_assert_cmpstr (action.destination, ==, "liked");
  g_signal_emit_by_name (button_for (GTK_WIDGET (page), "On repeat"), "clicked");
  g_assert_cmpstr (action.uri, ==, fixtures[5].uri);
  GtkScrolledWindow *outer = GTK_SCROLLED_WINDOW (find_type (GTK_WIDGET (page), GTK_TYPE_SCROLLED_WINDOW));
  GtkPolicyType horizontal, vertical;
  gtk_scrolled_window_get_policy (outer, &horizontal, &vertical);
  g_assert_cmpint (vertical, ==, GTK_POLICY_AUTOMATIC);
  GPtrArray *grids = spotifygtk_home_page_get_grids (page);
  guint scrolling_shelves = 0;
  for (guint i = 0; i < grids->len; i++) {
    GtkScrolledWindow *shelf = spotifygtk_album_grid_get_scroller (g_ptr_array_index (grids, i));
    gtk_scrolled_window_get_policy (shelf, &horizontal, &vertical);
    g_assert_cmpint (horizontal, ==, GTK_POLICY_EXTERNAL);
    g_assert_cmpint (vertical, ==, GTK_POLICY_NEVER);
    g_assert_false (gtk_widget_get_mapped (gtk_scrolled_window_get_hscrollbar (shelf)));
    GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment (shelf);
    /* Off-page shelves deliberately detach their model to free row trees. */
    if (gtk_adjustment_get_upper (adjustment) == 0) continue;
    g_assert_cmpfloat (gtk_adjustment_get_upper (adjustment), >, gtk_adjustment_get_page_size (adjustment));
    scrolling_shelves++;
    GtkWidget *heading = gtk_widget_get_first_child (gtk_widget_get_parent (
      GTK_WIDGET (g_ptr_array_index (grids, i))));
    GtkWidget *next_arrow = gtk_widget_get_last_child (heading);
    GtkWidget *previous_arrow = gtk_widget_get_prev_sibling (next_arrow);
    g_assert_true (gtk_widget_has_css_class (next_arrow, "home-shelf-arrow"));
    g_assert_true (gtk_widget_get_visible (next_arrow));
    g_assert_false (gtk_widget_get_sensitive (previous_arrow));
    g_signal_emit_by_name (next_arrow, "clicked");
    settle ();
    g_assert_cmpfloat (gtk_adjustment_get_value (adjustment), >, 0);
    g_assert_true (gtk_widget_get_sensitive (previous_arrow));
    g_signal_emit_by_name (previous_arrow, "clicked");
    settle ();
    g_assert_cmpfloat (gtk_adjustment_get_value (adjustment), ==, 0);
    gtk_adjustment_set_value (adjustment, 80);
    settle ();
    g_assert_cmpfloat (gtk_adjustment_get_value (adjustment), >, 0);
    gtk_adjustment_set_value (adjustment, 0);
  }
  g_assert_cmpuint (scrolling_shelves, >, 0);
  settle ();
  const SpotifyGtkTheme themes[] = { SPOTIFYGTK_THEME_LIGHT, SPOTIFYGTK_THEME_DARK, SPOTIFYGTK_THEME_DARK_PLUS };
  const gchar *previews[] = { "home-dashboard-light.png", "home-dashboard-dark.png", "home-dashboard-dark-plus.png" };
  for (guint i = 0; i < G_N_ELEMENTS (themes); i++) {
    apply_theme (themes[i]); settle ();
    assert_home_surfaces (window, first, g_ptr_array_index (grids, 1));
    save_preview (window, previews[i]);
  }
  gtk_window_set_default_size (GTK_WINDOW (window), 640, 850);
  settle ();
  g_assert_cmpint (gtk_widget_get_width (window), <=, 640);
  g_assert_cmpfloat (bounds (third, window).origin.y, >, bounds (first, window).origin.y);
  save_preview (window, "home-dashboard-narrow.png");
  g_signal_emit_by_name (button_for (gtk_widget_get_parent (primary), "See all  ›"), "clicked");
  settle ();
  AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window));
  g_assert_nonnull (dialog);
  g_assert_nonnull (find_label (GTK_WIDGET (dialog), "New releases for you"));
  adw_dialog_close (dialog); settle ();
  spotifygtk_home_page_clear_cache (page);
  g_assert_false (gtk_widget_get_mapped (first));
  g_assert_nonnull (find_label (GTK_WIDGET (page), "Your library"));
  g_free (action.uri);
  g_free (action.destination);
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_account_and_actions (void)
{
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 1200, 780);
  SpotifyGtkSettingsPage *page = spotifygtk_settings_page_new ();
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (page));
  gtk_window_present (GTK_WINDOW (window));
  settle ();
  g_assert_nonnull (find_label (GTK_WIDGET (page), "Account"));
  g_assert_null (find_label (GTK_WIDGET (page), "User"));
  g_assert_false (gtk_widget_get_sensitive (button_for (GTK_WIDGET (page), "Customize Home")));
  const gchar *actions[] = { "Log out", "Clear cache", "Reset", "Sign in" };
  for (guint i = 0; i < G_N_ELEMENTS (actions); i++) {
    GtkWidget *button = button_for (GTK_WIDGET (page), actions[i]);
    g_assert_false (gtk_widget_has_css_class (button, "pill-button"));
    g_assert_true (gtk_widget_has_css_class (button, "settings-action"));
  }
  spotifygtk_settings_page_set_account (page, "fixture-account");
  g_assert_nonnull (find_label (GTK_WIDGET (page), "Signed in"));
  g_assert_true (gtk_widget_get_visible (button_for (GTK_WIDGET (page), "Log out")));
  g_assert_false (gtk_widget_get_visible (button_for (GTK_WIDGET (page), "Sign in")));
  spotifygtk_settings_page_set_account (page, NULL);
  g_assert_false (gtk_widget_get_visible (button_for (GTK_WIDGET (page), "Log out")));
  g_assert_true (gtk_widget_get_visible (button_for (GTK_WIDGET (page), "Sign in")));
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_artist_follow_alignment (void)
{
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 1200, 780);
  SpotifyGtkArtistPage *page = spotifygtk_artist_page_new ();
  spotifygtk_artist_page_set_following (page, FALSE);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (page));
  gtk_window_present (GTK_WINDOW (window));
  settle ();
  GtkWidget *follow = button_for (GTK_WIDGET (page), "Follow");
  GtkWidget *hero = find_class (GTK_WIDGET (page), "artist-hero");
  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (spacer, -1, 1600);
  gtk_box_append (GTK_BOX (gtk_widget_get_parent (hero)), spacer);
  settle ();
  graphene_rect_t f = bounds (follow, window), h = bounds (hero, window);
  g_test_message ("Follow right %.1f, hero right %.1f", f.origin.x + f.size.width,
                  h.origin.x + h.size.width);
  g_assert_cmpfloat (fabs (f.origin.x + f.size.width - h.origin.x - h.size.width), <=, 1);
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_liked_mixed_dates (void)
{
  g_autoptr(SpotifyGtkLikedSongsPage) page = g_object_ref_sink (spotifygtk_liked_songs_page_new ());
  SpotifyNativeTrack local = { .uri = "local:track:fixture", .name = "Old local", .liked_at = 200 };
  SpotifyNativeTrack legacy = { .uri = "local:track:legacy", .name = "Undated local" };
  SpotifyNativeTrack newer = { .uri = "spotify:track:new", .name = "New Spotify" };
  SpotifyNativeTrack older = { .uri = "spotify:track:old", .name = "Old Spotify" };
  spotifygtk_liked_songs_page_add_track (page, &local);
  spotifygtk_liked_songs_page_add_track (page, &legacy);
  spotifygtk_liked_songs_page_add_track (page, &newer);
  spotifygtk_liked_songs_page_add_track (page, &older);
  g_autoptr(GHashTable) dates = g_hash_table_new (g_str_hash, g_str_equal);
  gint64 new_date = 300, old_date = 100;
  g_hash_table_insert (dates, newer.uri, &new_date);
  g_hash_table_insert (dates, older.uri, &old_date);
  spotifygtk_liked_songs_page_update_dates (page, dates, TRUE);
  g_autoptr(GPtrArray) tracks = spotifygtk_track_list_snapshot (
    spotifygtk_liked_songs_page_get_list (page));
  g_assert_cmpuint (tracks->len, ==, 4);
  g_assert_cmpstr (((SpotifyNativeTrack *) tracks->pdata[0])->uri, ==, newer.uri);
  g_assert_cmpstr (((SpotifyNativeTrack *) tracks->pdata[1])->uri, ==, local.uri);
  g_assert_cmpstr (((SpotifyNativeTrack *) tracks->pdata[2])->uri, ==, older.uri);
  g_assert_cmpstr (((SpotifyNativeTrack *) tracks->pdata[3])->uri, ==, legacy.uri);
  g_signal_emit_by_name (button_for (GTK_WIDGET (page), "Date added ↑"), "clicked");
  g_clear_pointer (&tracks, g_ptr_array_unref);
  tracks = spotifygtk_track_list_snapshot (spotifygtk_liked_songs_page_get_list (page));
  g_assert_cmpstr (((SpotifyNativeTrack *) tracks->pdata[0])->uri, ==, legacy.uri);
}

static void
test_overlay_delete_mapping (void)
{
  g_autoptr(SpotifyGtkTrackList) list = g_object_ref_sink (spotifygtk_track_list_new ());
  SpotifyNativeTrack a = { .uri = "local:track:a", .name = "A", .section_detail = "On this device", .device_index = 1 };
  SpotifyNativeTrack b = { .uri = "local:track:b", .name = "B", .section_detail = "On this device", .device_index = 2 };
  SpotifyNativeTrack c = { .uri = "local:track:c", .name = "C", .section_detail = "On this device", .device_index = 3 };
  g_autoptr(GPtrArray) rows = g_ptr_array_new ();
  g_ptr_array_add (rows, &c); g_ptr_array_add (rows, &a); g_ptr_array_add (rows, &b);
  spotifygtk_track_list_set_native_tracks (list, rows);
  g_assert_cmpint (spotifygtk_track_list_device_index_at (list, 0), ==, 2);
  g_assert_cmpint (spotifygtk_track_list_device_index_at (list, 1), ==, 0);
  spotifygtk_track_list_remove_position (list, 1);
  g_assert_cmpint (spotifygtk_track_list_device_index_at (list, 0), ==, 1);
  g_assert_cmpint (spotifygtk_track_list_device_index_at (list, 1), ==, 0);
}

static void
capture_navigation (SpotifyGtkNowPlayingPanel *panel, const gchar *uri,
                    const gchar *title, const gchar *kind, gpointer data)
{
  gchar **last = data;
  g_free (*last); *last = g_strdup (uri);
  g_assert_nonnull (title); g_assert_nonnull (kind);
  (void) panel;
}

static void
test_panel_navigation (void)
{
  g_autoptr(SpotifyGtkNowPlayingPanel) panel = g_object_ref_sink (spotifygtk_now_playing_panel_new ());
  SpotifyNativeTrack track = { .name = "Fixture", .artists = "Artist & artist", .album = "Album <fixture>",
    .artist_uri = "spotify:artist:0000000000000000000000",
    .album_uri = "spotify:album:0000000000000000000000" };
  spotifygtk_now_playing_panel_set_track (panel, track.name, track.artists, track.album);
  spotifygtk_now_playing_panel_set_navigation_track (panel, &track);
  GtkWidget *label = find_class (GTK_WIDGET (panel), "now-playing-metadata");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==, "Artist & artist • Album <fixture>");
  g_autofree gchar *last = NULL;
  g_signal_connect (panel, "context-requested", G_CALLBACK (capture_navigation), &last);
  gboolean handled = FALSE;
  g_signal_emit_by_name (label, "activate-link", "artist", &handled);
  g_assert_true (handled); g_assert_cmpstr (last, ==, track.artist_uri);
  g_signal_emit_by_name (label, "activate-link", "album", &handled);
  g_assert_cmpstr (last, ==, track.album_uri);
  track.artist_uri = "https://example.invalid";
  spotifygtk_now_playing_panel_set_navigation_track (panel, &track);
  g_clear_pointer (&last, g_free);
  g_signal_emit_by_name (label, "activate-link", "artist", &handled);
  g_assert_null (last);
  spotifygtk_now_playing_panel_set_track (panel, "Nothing playing", "", "");
  g_signal_emit_by_name (label, "activate-link", "album", &handled);
  g_assert_null (last);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_autofree gchar *isolated = g_dir_make_tmp ("spotifygtk-ui-test-XXXXXX", NULL);
  g_assert_nonnull (isolated);
  g_setenv ("XDG_CONFIG_HOME", isolated, TRUE);
  g_setenv ("XDG_DATA_HOME", isolated, TRUE);
  g_setenv ("XDG_CACHE_HOME", isolated, TRUE);
  if (!gtk_init_check ()) return 77;
#if GTK_CHECK_VERSION(4, 24, 0)
  g_object_set (gtk_settings_get_default (), "gtk-interface-color-scheme",
                 GTK_INTERFACE_COLOR_SCHEME_DEFAULT, NULL);
#else
  g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", FALSE, NULL);
#endif
  adw_init ();
  g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
  apply_theme (SPOTIFYGTK_THEME_DARK_PLUS);
  g_test_add_func ("/ui/library/local-empty-and-create", test_library_local_empty_and_create_position);
  g_test_add_func ("/ui/liked/sort-alignment-resize", test_liked_sort_alignment);
  g_test_add_func ("/ui/now-playing/collapse-style", test_right_collapse_style);
  g_test_add_func ("/ui/dialog/theme-palettes", test_dialog_palette);
  g_test_add_func ("/ui/artwork/borderless-centered", test_artwork_overlay);
  g_test_add_func ("/ui/home/cached-shelves", test_home_shelves);
  g_test_add_func ("/ui/home/large-feed", test_home_large_feed);
  g_test_add_func ("/ui/home/dashboard-responsive", test_home_dashboard);
  g_test_add_func ("/ui/cards/shared-library-search-design", test_shared_card_design);
  g_test_add_func ("/ui/settings/account-actions", test_account_and_actions);
  g_test_add_func ("/ui/artist/follow-alignment", test_artist_follow_alignment);
  g_test_add_func ("/ui/liked/mixed-dates", test_liked_mixed_dates);
  g_test_add_func ("/ui/playlist/overlay-delete-mapping", test_overlay_delete_mapping);
  g_test_add_func ("/ui/now-playing/navigation", test_panel_navigation);
  return g_test_run ();
}

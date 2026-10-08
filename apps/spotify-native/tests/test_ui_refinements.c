/* Exercise the actual application CSS and widgets without starting playback,
 * authentication, or a window session. All persistent state is temporary. */
#include "../src/ui/window.c"

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
  return g_test_run ();
}

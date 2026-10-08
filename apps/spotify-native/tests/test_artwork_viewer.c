#include "../src/ui/artwork_viewer.c"
#include "../src/ui/dialog_host.c"

/* Deterministic asynchronous loader: no network, account or disk cache. */
static GCancellable *pending_cancel;
static SpotifyCoverCallback pending_callback;
static gpointer pending_data;
static guint requests;
static gint requested_size;

void
spotifygtk_cover_load (const gchar *id, gint size, GCancellable *cancel,
                       SpotifyCoverCallback callback, gpointer data)
{
  g_assert_nonnull (id);
  g_assert_null (pending_cancel);
  pending_cancel = g_object_ref (cancel);
  pending_callback = callback;
  pending_data = data;
  requested_size = size;
  requests++;
}

static void
deliver (GdkTexture *texture)
{
  if (!g_cancellable_is_cancelled (pending_cancel))
    pending_callback (texture, pending_data);
  g_clear_object (&pending_cancel);
  pending_callback = NULL; pending_data = NULL;
}

static void
settle (void)
{
  gint64 until = g_get_monotonic_time () + 200000;
  while (g_get_monotonic_time () < until) {
    g_main_context_iteration (NULL, FALSE);
    g_usleep (1000);
  }
}

static GdkTexture *
fixture_texture (void)
{
  g_autoptr(GBytes) bytes = g_bytes_new_take (g_malloc0 (640 * 640 * 4), 640 * 640 * 4);
  return gdk_memory_texture_new (640, 640, GDK_MEMORY_R8G8B8A8, bytes, 640 * 4);
}

static GtkWidget *
make_window (void)
{
  GtkWidget *window = adw_application_window_new (NULL);
  spotifygtk_dialog_host_bind (ADW_APPLICATION_WINDOW (window));
  gtk_window_set_default_size (GTK_WINDOW (window), 1200, 950);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window), gtk_label_new ("Background"));
  gtk_window_present (GTK_WINDOW (window));
  settle ();
  return window;
}

static SpotifyGtkArtworkViewer *
open_viewer (GtkWidget *window)
{
  spotifygtk_artwork_viewer_present (window, "0000000000000000000000000000000000000000", "Artwork fixture");
  AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window));
  g_assert_nonnull (dialog);
  g_assert_cmpint (adw_dialog_get_presentation_mode (dialog), ==, ADW_DIALOG_FLOATING);
  g_assert_true (gtk_widget_get_root (GTK_WIDGET (dialog)) == GTK_ROOT (window));
  g_assert_true (gtk_widget_has_css_class (GTK_WIDGET (dialog), "artwork-viewer"));
  g_assert_cmpint (requested_size, >=, 532);
  g_assert_cmpint (requested_size, <=, ARTWORK_VIEWER_MAX_DECODE);
  return (SpotifyGtkArtworkViewer *) g_object_ref (dialog);
}

static void
test_release_on_close (void)
{
  GtkWidget *window = make_window ();
  for (guint i = 0; i < 16; i++) {
    g_autoptr(SpotifyGtkArtworkViewer) viewer = open_viewer (window);
    settle ();
    guint count = requests;
    spotifygtk_artwork_viewer_present (window, "another-image", "Must not stack");
    g_assert_cmpuint (requests, ==, count);
    GdkTexture *texture = fixture_texture ();
    gpointer alive = texture;
    g_object_add_weak_pointer (G_OBJECT (texture), &alive);
    deliver (texture);
    g_assert_true (gtk_picture_get_paintable (viewer->picture) == GDK_PAINTABLE (texture));
    g_assert_cmpint (adw_dialog_get_content_width (ADW_DIALOG (viewer)), <=, 640);
    settle (); /* Exercise a texture that has actually reached rendering. */
    g_object_unref (texture);
    /* No main-loop turn after closing: the viewer drops pixels at once,
     * even if its widget is retained by a caller or a closing animation. */
    adw_dialog_close (ADW_DIALOG (viewer));
    g_assert_true (viewer->closed);
    g_assert_null (gtk_picture_get_paintable (viewer->picture));
    g_assert_null (viewer->request);
    settle ();
    /* GTK's last submitted frame may temporarily own a render-node reference;
     * after that frame retires, even a retained dialog owns no image object. */
    g_assert_null (alive);
  }
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_close_during_load (void)
{
  GtkWidget *window = make_window ();
  g_autoptr(SpotifyGtkArtworkViewer) viewer = open_viewer (window);
  settle ();
  adw_dialog_close (ADW_DIALOG (viewer));
  g_assert_true (g_cancellable_is_cancelled (pending_cancel));
  g_autoptr(GdkTexture) texture = fixture_texture ();
  deliver (texture);
  g_assert_null (gtk_picture_get_paintable (viewer->picture));
  settle ();
  g_assert_null (adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window)));
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_failure_and_escape (void)
{
  GtkWidget *window = make_window ();
  g_autoptr(SpotifyGtkArtworkViewer) viewer = open_viewer (window);
  settle ();
  deliver (NULL);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (viewer->status)));
  g_assert_null (gtk_picture_get_paintable (viewer->picture));
  g_assert_true (on_dialog_escape (NULL, GDK_KEY_Escape, 0, 0, ADW_APPLICATION_WINDOW (window)));
  g_assert_true (viewer->closed);
  settle ();
  gtk_window_destroy (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ()) return 77;
#if GTK_CHECK_VERSION(4, 24, 0)
  g_object_set (gtk_settings_get_default (), "gtk-interface-color-scheme",
    GTK_INTERFACE_COLOR_SCHEME_DEFAULT, NULL);
#else
  g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", FALSE, NULL);
#endif
  adw_init ();
  g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
  g_test_add_func ("/artwork/release-16-previews", test_release_on_close);
  g_test_add_func ("/artwork/close-during-load", test_close_during_load);
  g_test_add_func ("/artwork/failure-escape", test_failure_and_escape);
  return g_test_run ();
}

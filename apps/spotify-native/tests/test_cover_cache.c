/* Exercise the actual asynchronous loader with no display or GTK widgets.
 * Block its network lane to reproduce offline/slow-CDN cache starvation. */
#include "../src/ui/cover_loader.c"

void spotifygtk_soup_session_configure_tls (SoupSession *session) { (void) session; }
void spotifygtk_catalog_cache_clear (void) {}
gchar *spotifygtk_local_catalog_dup_art_path (const gchar *id) { (void) id; return NULL; }

static GBytes *fixture_image (void)
{
  struct jpeg_compress_struct jpeg = {0};
  struct jpeg_error_mgr error;
  unsigned char *encoded = NULL;
  unsigned long length = 0;
  jpeg.err = jpeg_std_error (&error);
  jpeg_create_compress (&jpeg);
  jpeg_mem_dest (&jpeg, &encoded, &length);
  jpeg.image_width = jpeg.image_height = 8;
  jpeg.input_components = 3;
  jpeg.in_color_space = JCS_RGB;
  jpeg_set_defaults (&jpeg);
  jpeg_start_compress (&jpeg, TRUE);
  guchar pixels[8 * 3];
  memset (pixels, 127, sizeof pixels);
  while (jpeg.next_scanline < jpeg.image_height) {
    JSAMPROW row = pixels;
    jpeg_write_scanlines (&jpeg, &row, 1);
  }
  jpeg_finish_compress (&jpeg);
  jpeg_destroy_compress (&jpeg);
  GBytes *bytes = g_bytes_new (encoded, length);
  free (encoded);
  return bytes;
}

typedef struct { gboolean done; GdkTexture *texture; } Delivery;
static void delivered (GdkTexture *texture, gpointer data)
{
  Delivery *delivery = data;
  delivery->done = TRUE;
  delivery->texture = texture ? g_object_ref (texture) : NULL;
}
static gboolean deadline (gpointer data)
{
  *(gboolean *) data = TRUE;
  return G_SOURCE_REMOVE;
}
static void wait_delivery (Delivery *delivery)
{
  gboolean timed_out = FALSE;
  guint timer = g_timeout_add_seconds (5, deadline, &timed_out);
  while (!delivery->done && !timed_out)
    g_main_context_iteration (NULL, TRUE);
  if (!timed_out) g_source_remove (timer);
  g_assert_true (delivery->done);
  g_assert_nonnull (delivery->texture);
  g_clear_object (&delivery->texture);
  g_assert_cmpuint (cover_disk_active, ==, 0);
  g_assert_cmpuint (g_hash_table_size (in_flight), ==, 0);
}

static void test_offline_lane (void)
{
  g_autoptr(GBytes) image = fixture_image ();
  disk_cache_write ("abc123", image);
  cover_active = COVER_MAX_INFLIGHT; /* all download slots stalled */
  Delivery delivery = {0};
  spotifygtk_cover_load ("abc123", 96, NULL, delivered, &delivery);
  wait_delivery (&delivery);
  g_assert_cmpuint (cover_active, ==, COVER_MAX_INFLIGHT);
  g_assert_cmpuint (cover_cache_bytes, ==, 0);
  g_assert_cmpuint (g_hash_table_size (cover_cache), ==, 0);
  cover_active = 0;
}

static void test_url_alias (void)
{
  g_autoptr(GBytes) image = fixture_image ();
  const gchar *url = "https://i.scdn.co/image/def456";
  disk_cache_write (url, image);
  g_autofree gchar *id_path = disk_cache_path ("def456");
  g_autofree gchar *url_path = disk_cache_path (url);
  g_assert_cmpstr (id_path, ==, url_path);
  Delivery delivery = {0};
  spotifygtk_cover_load ("def456", 40, NULL, delivered, &delivery);
  wait_delivery (&delivery);
}

static void test_legacy_url_cache (void)
{
  const gchar *url = "https://i.scdn.co/image/012345";
  g_autofree gchar *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, url, -1);
  g_autofree gchar *legacy = g_build_filename (
    g_get_user_cache_dir (), "spotifygtk", "images", hash, NULL);
  g_autoptr(GBytes) image = fixture_image ();
  gsize size;
  const gchar *bytes = g_bytes_get_data (image, &size);
  g_assert_true (g_file_set_contents (legacy, bytes, size, NULL));
  Delivery delivery = {0};
  spotifygtk_cover_load ("012345", 40, NULL, delivered, &delivery);
  wait_delivery (&delivery);
}

static void test_cached_size_fallback (void)
{
  g_autoptr(GBytes) image = fixture_image ();
  disk_cache_write ("cafe", image);
  g_autofree gchar *chosen = spotifygtk_cover_select_source ("beef", "cafe");
  g_assert_cmpstr (chosen, ==, "cafe");
  disk_cache_write ("beef", image);
  g_autofree gchar *preferred = spotifygtk_cover_select_source ("beef", "cafe");
  g_assert_cmpstr (preferred, ==, "beef");
  g_autofree gchar *single = spotifygtk_cover_select_source (NULL, "cafe");
  g_assert_cmpstr (single, ==, "cafe");
}

static void test_cached_prefetch (void)
{
  g_autoptr(GBytes) image = fixture_image ();
  disk_cache_write ("faded", image);
  guint requests = cover_stats.misses;
  spotifygtk_cover_prefetch ("faded", 96);
  g_assert_cmpuint (cover_stats.misses, ==, requests);
  g_assert_cmpuint (cover_disk_active, ==, 0);
}

static void test_restart (void)
{
  if (g_test_subprocess ()) {
    cover_active = COVER_MAX_INFLIGHT;
    Delivery delivery = {0};
    spotifygtk_cover_load ("accede", 40, NULL, delivered, &delivery);
    wait_delivery (&delivery);
    cover_active = 0;
    return;
  }
  g_autoptr(GBytes) image = fixture_image ();
  disk_cache_write ("accede", image);
  g_test_trap_subprocess (NULL, 10 * G_USEC_PER_SEC, 0);
  g_test_trap_assert_passed ();
}

static void test_cancelled_subscriber (void)
{
  g_autoptr(GBytes) image = fixture_image ();
  disk_cache_write ("decade", image);
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  Delivery stale = {0}, live = {0};
  spotifygtk_cover_load ("decade", 40, cancel, delivered, &stale);
  spotifygtk_cover_load ("decade", 40, NULL, delivered, &live);
  g_cancellable_cancel (cancel);
  wait_delivery (&live);
  g_assert_false (stale.done);
}

static void test_corrupt_entry_retries (void)
{
  g_autofree gchar *path = disk_cache_path ("badcafe");
  g_assert_true (g_file_set_contents (path, "invalid", 7, NULL));
  cover_active = COVER_MAX_INFLIGHT;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  Delivery delivery = {0};
  spotifygtk_cover_load ("badcafe", 96, cancel, delivered, &delivery);
  gboolean timed_out = FALSE;
  guint timer = g_timeout_add_seconds (5, deadline, &timed_out);
  while (cover_disk_active && !timed_out)
    g_main_context_iteration (NULL, TRUE);
  if (!timed_out) g_source_remove (timer);
  g_assert_false (timed_out);
  g_assert_false (delivery.done); /* original waiter survives the retry */
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  g_assert_cmpuint (g_queue_get_length (&cover_queue), ==, 1);
  g_cancellable_cancel (cancel);
  cover_active = 0;
  cover_pump ();
  g_assert_cmpuint (g_queue_get_length (&cover_queue), ==, 0);
  g_assert_cmpuint (g_hash_table_size (in_flight), ==, 0);
}

static void test_finished_download_survives_cancel (void)
{
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_cancellable_cancel (cancel);
  PendingRequest *request = g_new0 (PendingRequest, 1);
  request->callback = delivered;
  request->cancellable = g_object_ref (cancel);
  const gchar *key = "face@96";
  g_hash_table_insert (in_flight, g_strdup (key), g_list_append (NULL, request));
  FetchClosure *fetch = g_new0 (FetchClosure, 1);
  fetch->cache_key = g_strdup (key);
  fetch->target_px = 96;
  cover_active++;
  finish_cover_fetch (fetch, fixture_image (), NULL);
  gboolean timed_out = FALSE;
  guint timer = g_timeout_add_seconds (5, deadline, &timed_out);
  while (cover_active && !timed_out)
    g_main_context_iteration (NULL, TRUE);
  if (!timed_out) g_source_remove (timer);
  g_assert_false (timed_out);
  g_autofree gchar *path = disk_cache_read_path ("face");
  g_assert_nonnull (path);
  g_assert_cmpuint (g_hash_table_size (in_flight), ==, 0);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_autofree gchar *root = NULL;
  if (!g_test_subprocess ()) {
    root = g_dir_make_tmp ("spotifygtk-art-cache-XXXXXX", NULL);
    g_assert_nonnull (root);
    g_setenv ("XDG_CACHE_HOME", root, TRUE);
    g_setenv ("XDG_CONFIG_HOME", root, TRUE);
  }
  ensure_initialised ();
  g_test_add_func ("/art-cache/offline-independent-lane", test_offline_lane);
  g_test_add_func ("/art-cache/url-id-alias", test_url_alias);
  g_test_add_func ("/art-cache/legacy-url-hash", test_legacy_url_cache);
  g_test_add_func ("/art-cache/cached-size-fallback", test_cached_size_fallback);
  g_test_add_func ("/art-cache/cached-prefetch-no-decode", test_cached_prefetch);
  g_test_add_func ("/art-cache/restart", test_restart);
  g_test_add_func ("/art-cache/cancelled-subscriber", test_cancelled_subscriber);
  g_test_add_func ("/art-cache/corrupt-entry-retries", test_corrupt_entry_retries);
  g_test_add_func ("/art-cache/finished-download-survives-cancel", test_finished_download_survives_cancel);
  gint result = g_test_run ();
  if (root) {
    g_autoptr(GFile) directory = g_file_new_for_path (root);
    g_assert_true (delete_cache_tree (directory, NULL, NULL));
  }
  return result;
}

#include <adwaita.h>

#include "ui/playback_bar.h"
#include "ui/context_menu.h"
#include "ui/cover_loader.h"
#include "spotify/track_meta.h"

/* The shuffle control does not request artwork, metadata or a context menu.
 * Keep this test focused on the real playback bar and its click signal. */
void
spotifygtk_cover_load_playback (const gchar *cover_id, gint target_px,
                                GCancellable *cancellable,
                                SpotifyCoverCallback callback,
                                gpointer user_data)
{
  (void) cover_id; (void) target_px; (void) cancellable;
  callback (NULL, user_data);
}

gchar *
spotifygtk_track_share_url (const gchar *track_uri)
{
  (void) track_uri;
  return NULL;
}

SpotifyGtkContextMenu *
spotifygtk_context_menu_new (void)
{
  return NULL;
}

void
spotifygtk_context_menu_add (SpotifyGtkContextMenu *menu,
                             const gchar *label, gboolean enabled,
                             const gchar *tooltip, GCallback callback,
                             gpointer user_data)
{
  (void) menu; (void) label; (void) enabled; (void) tooltip;
  (void) callback; (void) user_data;
}

void
spotifygtk_context_menu_present (SpotifyGtkContextMenu *menu,
                                 GtkWidget *anchor, gdouble x, gdouble y,
                                 gpointer context, GDestroyNotify context_free)
{
  (void) menu; (void) anchor; (void) x; (void) y;
  if (context_free)
    context_free (context);
}

gpointer
spotifygtk_context_menu_get_context (GtkWidget *entry_button)
{
  (void) entry_button;
  return NULL;
}

GtkPopover *
spotifygtk_context_menu_get_popover (GtkWidget *entry_button)
{
  (void) entry_button;
  return NULL;
}

static GtkButton *
find_shuffle_button (GtkWidget *widget)
{
  if (GTK_IS_BUTTON (widget) &&
      g_strcmp0 (gtk_widget_get_tooltip_text (widget), "Enable shuffle") == 0)
    return GTK_BUTTON (widget);

  for (GtkWidget *child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child)) {
    GtkButton *button = find_shuffle_button (child);
    if (button)
      return button;
  }
  return NULL;
}

typedef struct {
  gboolean has_local_tracks;
  guint requested[3];
  guint count;
} ShuffleFixture;

static gboolean gtk_available;

static void
test_context_mode_resolution (void)
{
  g_assert_cmpint (spotifygtk_shuffle_mode_for_context (
                     SPOTIFYGTK_SHUFFLE_OFF, TRUE), ==,
                   SPOTIFYGTK_SHUFFLE_OFF);
  g_assert_cmpint (spotifygtk_shuffle_mode_for_context (
                     SPOTIFYGTK_SHUFFLE_NORMAL, TRUE), ==,
                   SPOTIFYGTK_SHUFFLE_NORMAL);
  g_assert_cmpint (spotifygtk_shuffle_mode_for_context (
                     SPOTIFYGTK_SHUFFLE_SMART, TRUE), ==,
                   SPOTIFYGTK_SHUFFLE_OFF);
  g_assert_cmpint (spotifygtk_shuffle_mode_for_context (
                     SPOTIFYGTK_SHUFFLE_SMART, FALSE), ==,
                   SPOTIFYGTK_SHUFFLE_SMART);
}

static void
on_shuffle_mode_changed (SpotifyGtkPlaybackBar *bar, guint requested,
                         gpointer user_data)
{
  ShuffleFixture *fixture = user_data;
  g_assert_cmpuint (fixture->count, <, G_N_ELEMENTS (fixture->requested));
  fixture->requested[fixture->count++] = requested;

  /* Mirror the window's resolution of an unsupported Smart request. The
   * emitted request and the resulting button state are both observable. */
  SpotifyGtkShuffleMode resolved = spotifygtk_shuffle_mode_for_context (
    requested, fixture->has_local_tracks);
  if (resolved != requested)
    spotifygtk_playback_bar_set_modes (bar, resolved, SPOTIFYGTK_REPEAT_OFF);
}

static void
test_local_context_disables_shuffle (void)
{
  if (!gtk_available) {
    g_test_skip ("GTK display unavailable");
    return;
  }

  SpotifyGtkPlaybackBar *bar = spotifygtk_playback_bar_new ();
  g_object_ref_sink (bar);
  spotifygtk_playback_bar_set_modes (bar, SPOTIFYGTK_SHUFFLE_OFF,
                                    SPOTIFYGTK_REPEAT_OFF);
  GtkButton *button = find_shuffle_button (GTK_WIDGET (bar));
  g_assert_nonnull (button);

  ShuffleFixture fixture = { .has_local_tracks = TRUE };
  g_signal_connect (bar, "shuffle-mode-changed",
                    G_CALLBACK (on_shuffle_mode_changed), &fixture);

  g_signal_emit_by_name (button, "clicked");
  g_assert_cmpuint (fixture.count, ==, 1);
  g_assert_cmpuint (fixture.requested[0], ==, SPOTIFYGTK_SHUFFLE_NORMAL);
  g_assert_true (gtk_widget_has_css_class (GTK_WIDGET (button), "toggle-active"));

  /* The next click requests Smart, which this mixed/local context cannot
   * use. It must reach Off instead of resetting to Normal indefinitely. */
  g_signal_emit_by_name (button, "clicked");
  g_assert_cmpuint (fixture.count, ==, 2);
  g_assert_cmpuint (fixture.requested[1], ==, SPOTIFYGTK_SHUFFLE_SMART);
  g_assert_false (gtk_widget_has_css_class (GTK_WIDGET (button), "toggle-active"));
  g_assert_cmpstr (gtk_widget_get_tooltip_text (GTK_WIDGET (button)), ==,
                   "Enable shuffle");

  g_object_unref (bar);
}

static void
test_spotify_context_preserves_smart (void)
{
  if (!gtk_available) {
    g_test_skip ("GTK display unavailable");
    return;
  }

  SpotifyGtkPlaybackBar *bar = spotifygtk_playback_bar_new ();
  g_object_ref_sink (bar);
  spotifygtk_playback_bar_set_modes (bar, SPOTIFYGTK_SHUFFLE_OFF,
                                    SPOTIFYGTK_REPEAT_OFF);
  GtkButton *button = find_shuffle_button (GTK_WIDGET (bar));
  g_assert_nonnull (button);

  ShuffleFixture fixture = { .has_local_tracks = FALSE };
  g_signal_connect (bar, "shuffle-mode-changed",
                    G_CALLBACK (on_shuffle_mode_changed), &fixture);

  g_signal_emit_by_name (button, "clicked");
  g_signal_emit_by_name (button, "clicked");
  g_assert_cmpuint (fixture.count, ==, 2);
  g_assert_cmpuint (fixture.requested[0], ==, SPOTIFYGTK_SHUFFLE_NORMAL);
  g_assert_cmpuint (fixture.requested[1], ==, SPOTIFYGTK_SHUFFLE_SMART);
  g_assert_true (gtk_widget_has_css_class (GTK_WIDGET (button), "toggle-active"));
  g_assert_true (gtk_widget_has_css_class (GTK_WIDGET (button), "smart-shuffle"));

  g_signal_emit_by_name (button, "clicked");
  g_assert_cmpuint (fixture.count, ==, 3);
  g_assert_cmpuint (fixture.requested[2], ==, SPOTIFYGTK_SHUFFLE_OFF);
  g_assert_false (gtk_widget_has_css_class (GTK_WIDGET (button), "toggle-active"));

  g_object_unref (bar);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gtk_available = gtk_init_check ();
  g_test_add_func ("/shuffle/context-mode-resolution", test_context_mode_resolution);
  g_test_add_func ("/shuffle/local-context-disables", test_local_context_disables_shuffle);
  g_test_add_func ("/shuffle/spotify-context-cycles", test_spotify_context_preserves_smart);
  return g_test_run ();
}

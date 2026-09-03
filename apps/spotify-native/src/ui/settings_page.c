/*
 * settings_page.c — Settings page implementation.
 *
 * Grouped rows in the libadwaita idiom. Options that are not implemented yet
 * are present but insensitive, with a note saying so: the point of listing
 * them is to show the intended shape, and a control that silently does
 * nothing is worse than one that says it cannot.
 */

#include "settings_page.h"

#include <glib/gstdio.h>
#include "settings.h"
#include "../audio/dsp.h"
#include "eq_graph.h"
#include "smooth_scroll.h"

struct _SpotifyGtkSettingsPage {
  GtkBox parent_instance;
  SpotifyGtkSettings *settings;
  SpotifyGtkEqGraph  *eq_graph;
  GtkWidget          *account_name;   /* filled once the session knows who */
};

/* The stored array and the filter must agree on how many bands there are. */
G_STATIC_ASSERT (SPOTIFYGTK_SETTINGS_EQ_BANDS == SPOTIFYGTK_EQ_BANDS);

G_DEFINE_FINAL_TYPE (SpotifyGtkSettingsPage, spotifygtk_settings_page, GTK_TYPE_BOX)

enum { LOG_OUT, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
spotifygtk_settings_page_class_init (SpotifyGtkSettingsPageClass *klass)
{
  /* The page does not own the session or the credentials, so it asks rather
   * than acts -- the window owns both and does the actual sign-out. */
  signals[LOG_OUT] = g_signal_new ("log-out", G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
on_log_out_clicked (GtkButton *button, gpointer user_data)
{
  g_signal_emit (SPOTIFYGTK_SETTINGS_PAGE (user_data), signals[LOG_OUT], 0);
  (void) button;
}

/* === About === */

#define REPO_URL "https://github.com/martiancomputer/SpotifyGTK"

/*
 * When this install was first run.
 *
 * Stamped on first launch rather than read from the binary's mtime: a rebuild
 * rewrites the binary, so mtime answers "when was this last compiled", which
 * is not the question. The stamp is written once and then only read, so it
 * survives every later build.
 *
 * Returns a display string; the caller frees it.
 */
/*
 * Fold `path` and anything directly inside it into the oldest mtime seen.
 *
 * Both the config and the cache directory are considered. The config one alone
 * is misleading: the credentials file is rewritten on every token refresh, so
 * its mtime is always recent. The cover cache is only ever added to, so its
 * directory keeps the date it was created -- which is the first run.
 */
static void
fold_oldest_mtime (const gchar *path, gint64 *oldest)
{
  GStatBuf st;
  if (g_stat (path, &st) == 0 && st.st_mtime > 0 &&
      (*oldest == 0 || (gint64) st.st_mtime < *oldest))
    *oldest = (gint64) st.st_mtime;

  g_autoptr(GDir) d = g_dir_open (path, 0, NULL);
  const gchar *name;
  while (d && (name = g_dir_read_name (d)) != NULL) {
    g_autofree gchar *child = g_build_filename (path, name, NULL);
    if (g_stat (child, &st) == 0 && st.st_mtime > 0 &&
        (*oldest == 0 || (gint64) st.st_mtime < *oldest))
      *oldest = (gint64) st.st_mtime;
  }
}

static gchar *
installed_on (void)
{
  g_autofree gchar *dir = g_build_filename (g_get_user_config_dir (),
                                            "spotify-native", NULL);
  g_autofree gchar *path = g_build_filename (dir, "installed", NULL);

  g_autofree gchar *stamp = NULL;
  if (!g_file_get_contents (path, &stamp, NULL, NULL)) {
    /*
     * No stamp yet. On a fresh install that is because this is the first run,
     * and now is the right answer -- but on an install that predates this
     * feature it would claim the client appeared the moment About was added.
     *
     * So seed from the oldest thing in the config directory instead. The
     * credentials written at first sign-in are usually the earliest, which is
     * as close to "installed" as anything on disk gets. Falls back to now when
     * the directory is empty, which is the fresh-install case.
     */
    g_mkdir_with_parents (dir, 0700);

    g_autofree gchar *cache = g_build_filename (g_get_user_cache_dir (),
                                                "spotifygtk", NULL);
    gint64 oldest = 0;
    fold_oldest_mtime (dir, &oldest);
    fold_oldest_mtime (cache, &oldest);

    g_autoptr(GDateTime) seed = oldest > 0
      ? g_date_time_new_from_unix_local (oldest)
      : g_date_time_new_now_local ();
    stamp = g_date_time_format_iso8601 (seed);
    if (!g_file_set_contents (path, stamp, -1, NULL))
      return g_strdup ("Unknown");
  }

  g_autoptr(GDateTime) when = g_date_time_new_from_iso8601 (g_strstrip (stamp), NULL);
  if (!when)
    return g_strdup ("Unknown");

  /* Local time, spelled out -- this is read once out of curiosity, not
   * scanned, so it is worth being unambiguous rather than compact. */
  return g_date_time_format (when, "%e %B %Y at %H:%M");
}

/* === Building blocks === */

static GtkWidget *
build_group (const gchar *title)
{
  GtkWidget *group = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);

  GtkWidget *label = gtk_label_new (title);
  gtk_widget_add_css_class (label, "section-heading");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (group), label);

  return group;
}

/* One row: title on the left, control on the right, optional subtitle. */
static GtkWidget *
build_row (const gchar *title, const gchar *subtitle, GtkWidget *control)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_add_css_class (row, "card");
  gtk_widget_set_margin_start (row, 0);
  gtk_widget_set_margin_end (row, 0);

  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_margin_start (text, 16);
  gtk_widget_set_margin_top (text, 12);
  gtk_widget_set_margin_bottom (text, 12);

  GtkWidget *title_label = gtk_label_new (title);
  gtk_widget_add_css_class (title_label, "normal-text");
  gtk_label_set_xalign (GTK_LABEL (title_label), 0.0);
  gtk_box_append (GTK_BOX (text), title_label);

  if (subtitle) {
    GtkWidget *sub = gtk_label_new (subtitle);
    gtk_widget_add_css_class (sub, "dim-text");
    gtk_label_set_xalign (GTK_LABEL (sub), 0.0);
    gtk_label_set_wrap (GTK_LABEL (sub), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (sub), 62);
    gtk_box_append (GTK_BOX (text), sub);
  }

  gtk_box_append (GTK_BOX (row), text);

  gtk_widget_set_valign (control, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (control, 16);
  gtk_box_append (GTK_BOX (row), control);

  return row;
}

static GtkWidget *
build_dropdown (const gchar * const *options, guint selected, gboolean enabled)
{
  GtkWidget *dropdown = gtk_drop_down_new_from_strings (options);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (dropdown), selected);
  gtk_widget_set_sensitive (dropdown, enabled);
  if (!enabled)
    gtk_widget_set_tooltip_text (dropdown, "Not implemented yet");
  return dropdown;
}

/* === Handlers === */

static void
on_media_mode_changed (GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  guint selected = gtk_drop_down_get_selected (dropdown);

  /* Order matches the strings below: 0 = Media, 1 = Text only. */
  spotifygtk_settings_set_media_mode (self->settings,
    selected == 0 ? SPOTIFYGTK_MEDIA_FULL : SPOTIFYGTK_MEDIA_TEXT_ONLY);
  (void) pspec;
}

static void
on_sample_rate_changed (GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  spotifygtk_settings_set_sample_rate (self->settings,
    (SpotifyGtkSampleRate) gtk_drop_down_get_selected (dropdown));
  (void) pspec;
}

static void
on_theme_changed (GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  spotifygtk_settings_set_theme (self->settings,
                                 (SpotifyGtkTheme) gtk_drop_down_get_selected (dropdown));
  (void) pspec;
}

static void
on_renderer_changed (GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  spotifygtk_settings_set_renderer (
    self->settings,
    (SpotifyGtkRenderer) gtk_drop_down_get_selected (dropdown));
  (void) pspec;
}


/* === Equaliser === */


static void
on_eq_enabled_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  spotifygtk_settings_set_eq_enabled (self->settings, gtk_switch_get_active (sw));
  (void) pspec;
}


static void
on_eq_reset_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  spotifygtk_settings_reset_eq (self->settings);

  /* One write, then redraw the curve flat. The graph does not emit for a
   * programmatic set, so this cannot loop back through on_eq_band_changed. */
  if (self->eq_graph)
    spotifygtk_eq_graph_set_gains (self->eq_graph,
                                   spotifygtk_settings_get_eq_gains (self->settings));
  (void) button;
}

static void
on_eq_graph_band_changed (SpotifyGtkEqGraph *graph, guint band, gdouble gain_db,
                          gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  spotifygtk_settings_set_eq_band (self->settings, band, gain_db);
  (void) graph;
}

/* A 15-band graphic EQ: an enable switch, a reset, and the response curve
 * itself, dragged directly. The curve replaced a strip of vertical sliders --
 * see eq_graph.c for why showing the summed response beats showing 15
 * independent handle positions. */
static GtkWidget *
build_equalizer (SpotifyGtkSettingsPage *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (box, "card");
  gtk_widget_set_margin_top (box, 4);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (head, 16);
  gtk_widget_set_margin_end (head, 16);
  gtk_widget_set_margin_top (head, 12);

  GtkWidget *sw = gtk_switch_new ();
  gtk_switch_set_active (GTK_SWITCH (sw),
                         spotifygtk_settings_get_eq_enabled (self->settings));
  gtk_widget_set_valign (sw, GTK_ALIGN_CENTER);
  g_signal_connect (sw, "notify::active", G_CALLBACK (on_eq_enabled_toggled), self);
  gtk_box_append (GTK_BOX (head), sw);

  GtkWidget *on_label = gtk_label_new ("Enabled");
  gtk_widget_add_css_class (on_label, "normal-text");
  gtk_widget_set_hexpand (on_label, TRUE);
  gtk_label_set_xalign (GTK_LABEL (on_label), 0.0);
  gtk_box_append (GTK_BOX (head), on_label);

  GtkWidget *reset = gtk_button_new_with_label ("Reset");
  gtk_widget_add_css_class (reset, "pill-button");
  g_signal_connect (reset, "clicked", G_CALLBACK (on_eq_reset_clicked), self);
  gtk_box_append (GTK_BOX (head), reset);
  gtk_box_append (GTK_BOX (box), head);

  self->eq_graph = spotifygtk_eq_graph_new ();
  spotifygtk_eq_graph_set_gains (self->eq_graph,
                                 spotifygtk_settings_get_eq_gains (self->settings));
  g_signal_connect (self->eq_graph, "band-changed",
                    G_CALLBACK (on_eq_graph_band_changed), self);
  gtk_widget_set_margin_start (GTK_WIDGET (self->eq_graph), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self->eq_graph), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->eq_graph), 12);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->eq_graph));

  return box;
}

/* === Construction === */

static void
spotifygtk_settings_page_init (SpotifyGtkSettingsPage *self)
{
  self->settings = spotifygtk_settings_get_default ();

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  spotifygtk_smooth_scroll_attach (GTK_SCROLLED_WINDOW (scroller),
                                   GTK_ORIENTATION_VERTICAL);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 28);
  gtk_widget_set_margin_start (content, 34);
  gtk_widget_set_margin_end (content, 24);
  gtk_widget_set_margin_top (content, 22);
  gtk_widget_set_margin_bottom (content, 24);

  GtkWidget *title = gtk_label_new ("Settings");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (content), title);

  /* ── Interface ─────────────────────────────────────────────── */
  GtkWidget *interface_group = build_group ("Interface");

  static const gchar * const themes[] = { "Dark", "White", "Milk", NULL };
  GtkWidget *theme_dd = build_dropdown (themes,
                                        spotifygtk_settings_get_theme (self->settings),
                                        TRUE);
  g_signal_connect (theme_dd, "notify::selected", G_CALLBACK (on_theme_changed), self);
  gtk_box_append (GTK_BOX (interface_group),
                  build_row ("Theme",
                             "Dark (default), White, or the warmer Milk. "
                             "Applies immediately and is remembered.",
                             theme_dd));

  /* The one setting on this page that is fully wired. */
  static const gchar * const media_modes[] = { "Media", "Text only", NULL };
  GtkWidget *media_dd = build_dropdown (
    media_modes,
    spotifygtk_settings_get_media_mode (self->settings) == SPOTIFYGTK_MEDIA_FULL ? 0 : 1,
    TRUE);
  g_signal_connect (media_dd, "notify::selected", G_CALLBACK (on_media_mode_changed), self);
  gtk_box_append (GTK_BOX (interface_group),
                  build_row ("Previews",
                             "Media shows album artwork. Text only hides it "
                             "everywhere and skips downloading it at all.",
                             media_dd));

  gtk_box_append (GTK_BOX (content), interface_group);

  /* ── Audio ─────────────────────────────────────────────────── */
  GtkWidget *audio_group = build_group ("Audio");

  static const gchar * const rates[] = { "Default", "44.1 kHz", "48 kHz", "96 kHz", NULL };
  GtkWidget *rate_dd = build_dropdown (rates,
    (guint) spotifygtk_settings_get_sample_rate (self->settings), TRUE);
  g_signal_connect (rate_dd, "notify::selected",
                    G_CALLBACK (on_sample_rate_changed), self);
  gtk_box_append (GTK_BOX (audio_group),
                  build_row ("Sample rate",
                             "Converts the 44.1 kHz stream to the chosen device "
                             "rate. Default follows the stream and does no "
                             "conversion, which is the cleanest path.",
                             rate_dd));

  static const gchar * const formats[] = { "Native", "24-bit", NULL };
  gtk_box_append (GTK_BOX (audio_group),
                  build_row ("Sample format",
                             "Decoded output is 16-bit signed PCM; 24-bit "
                             "needs a converter in the output path.",
                             build_dropdown (formats, 0, FALSE)));

  static const gchar * const resamplers[] = { "Native", NULL };
  gtk_box_append (GTK_BOX (audio_group),
                  build_row ("Resampler",
                             "No resampling happens yet — samples reach the "
                             "device at the rate they were decoded.",
                             build_dropdown (resamplers, 0, FALSE)));

  GtkWidget *eq_heading = gtk_label_new ("Equalizer");
  gtk_widget_add_css_class (eq_heading, "normal-text");
  gtk_label_set_xalign (GTK_LABEL (eq_heading), 0.0);
  gtk_widget_set_margin_top (eq_heading, 4);
  gtk_box_append (GTK_BOX (audio_group), eq_heading);
  gtk_box_append (GTK_BOX (audio_group), build_equalizer (self));

  gtk_box_append (GTK_BOX (content), audio_group);

  /* ── Performance ───────────────────────────────────────────── */
  GtkWidget *perf_group = build_group ("Performance");

  static const gchar * const renderers[] = {
    "Automatic", "Vulkan (experimental)", "OpenGL", "Cairo (software)", NULL
  };
  GtkWidget *renderer_dd = build_dropdown (
    renderers, spotifygtk_settings_get_renderer (self->settings), TRUE);
  g_signal_connect (renderer_dd, "notify::selected",
                    G_CALLBACK (on_renderer_changed), self);
  gtk_box_append (GTK_BOX (perf_group),
                  build_row ("Renderer",
                             "Automatic lets GTK choose. A forced backend "
                             "takes effect after restarting; GSK_RENDERER "
                             "from the environment always takes precedence.",
                             renderer_dd));

  gtk_box_append (GTK_BOX (content), perf_group);

  /* ── User ──────────────────────────────────────────────────── */
  GtkWidget *user_group = build_group ("User");

  /*
   * Who is signed in, above the button that signs them out -- which is the
   * one place it matters, since this client can hold either of two accounts
   * and the log-out button gives no clue which it is about to forget.
   *
   * The Spotify user id, because that is what the client actually has. A
   * display name and a picture live behind the Web API, which nothing here
   * speaks; that is its own piece of work rather than something to fake.
   */
  self->account_name = gtk_label_new ("Not signed in");
  gtk_widget_add_css_class (self->account_name, "normal-text");
  gtk_label_set_xalign (GTK_LABEL (self->account_name), 1.0);
  gtk_label_set_ellipsize (GTK_LABEL (self->account_name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign (self->account_name, GTK_ALIGN_CENTER);

  gtk_box_append (GTK_BOX (user_group),
                  build_row ("Account",
                             "The Spotify account this client is signed in as.",
                             self->account_name));
  gtk_box_append (GTK_BOX (content), user_group);

  /* --- Account --- */
  GtkWidget *account_group = build_group ("Account");

  GtkWidget *logout = gtk_button_new_with_label ("Log out");
  gtk_widget_add_css_class (logout, "pill-button");
  g_signal_connect (logout, "clicked", G_CALLBACK (on_log_out_clicked), self);
  gtk_box_append (GTK_BOX (account_group),
                  build_row ("Signed in",
                             "Forgets the stored credentials and returns to the "
                             "sign-in screen. Playback stops.",
                             logout));

  gtk_box_append (GTK_BOX (content), account_group);

  /* --- About --- */
  GtkWidget *about_group = build_group ("About");

  /*
   * A real link rather than a button: it can be middle-clicked, copied and
   * read without being activated, which a button cannot.
   */
  GtkWidget *repo = gtk_link_button_new_with_label (REPO_URL, "Open on GitHub");
  gtk_widget_add_css_class (repo, "pill-button");
  gtk_widget_set_valign (repo, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (about_group),
                  build_row ("Repository",
                             "SpotifyGTK is a native Spotify client for Linux, "
                             "written in C and licensed GPLv3. Source, issues "
                             "and releases live here.",
                             repo));

  g_autofree gchar *since = installed_on ();
  GtkWidget *since_label = gtk_label_new (since);
  gtk_widget_add_css_class (since_label, "dim-text");
  gtk_widget_set_valign (since_label, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (since_label, 16);
  gtk_box_append (GTK_BOX (about_group),
                  build_row ("Installed",
                             "When this copy was first run. Version numbers "
                             "will join it here once there is something to "
                             "number.",
                             since_label));

  gtk_box_append (GTK_BOX (content), about_group);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), content);
  gtk_box_append (GTK_BOX (self), scroller);
}

void
spotifygtk_settings_page_set_account (SpotifyGtkSettingsPage *self,
                                      const gchar            *username)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS_PAGE (self));
  if (!self->account_name)
    return;
  gtk_label_set_text (GTK_LABEL (self->account_name),
                      (username && *username) ? username : "Not signed in");
}

SpotifyGtkSettingsPage *
spotifygtk_settings_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_SETTINGS_PAGE, NULL);
}

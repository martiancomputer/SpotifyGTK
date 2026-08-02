/*
 * settings_page.c — Settings page implementation.
 *
 * Grouped rows in the libadwaita idiom. Options that are not implemented yet
 * are present but insensitive, with a note saying so: the point of listing
 * them is to show the intended shape, and a control that silently does
 * nothing is worse than one that says it cannot.
 */

#include "settings_page.h"
#include "settings.h"
#include "../audio/dsp.h"
#include "eq_graph.h"
#include "smooth_scroll.h"

struct _SpotifyGtkSettingsPage {
  GtkBox parent_instance;
  SpotifyGtkSettings *settings;
  SpotifyGtkEqGraph  *eq_graph;
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

  static const gchar * const renderers[] = { "GTK", "Vulkan (experimental)", NULL };
  gtk_box_append (GTK_BOX (perf_group),
                  build_row ("Renderer",
                             "GTK's own renderer. Vulkan compositing has not "
                             "been started.",
                             build_dropdown (renderers, 0, FALSE)));

  gtk_box_append (GTK_BOX (content), perf_group);

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

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), content);
  gtk_box_append (GTK_BOX (self), scroller);
}

SpotifyGtkSettingsPage *
spotifygtk_settings_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_SETTINGS_PAGE, NULL);
}

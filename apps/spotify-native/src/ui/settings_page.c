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

struct _SpotifyGtkSettingsPage {
  GtkBox parent_instance;
  SpotifyGtkSettings *settings;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkSettingsPage, spotifygtk_settings_page, GTK_TYPE_BOX)

static void
spotifygtk_settings_page_class_init (SpotifyGtkSettingsPageClass *klass)
{
  (void) klass;
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
on_theme_changed (GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  spotifygtk_settings_set_theme (self->settings,
                                 (SpotifyGtkTheme) gtk_drop_down_get_selected (dropdown));
  (void) pspec;
}


/* === Equaliser === */

static gchar *
format_freq (gint hz)
{
  if (hz >= 1000)
    return g_strdup_printf ("%gk", hz / 1000.0);
  return g_strdup_printf ("%d", hz);
}

static void
on_eq_enabled_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  spotifygtk_settings_set_eq_enabled (self->settings, gtk_switch_get_active (sw));
  (void) pspec;
}

static void
on_eq_band_changed (GtkRange *range, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  guint band = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (range), "band"));
  spotifygtk_settings_set_eq_band (self->settings, band, gtk_range_get_value (range));
}

static void
on_eq_reset_clicked (GtkButton *button, gpointer user_data)
{
  SpotifyGtkSettingsPage *self = user_data;
  spotifygtk_settings_reset_eq (self->settings);

  /* Snap the sliders back; blocking the handler so the reset is one action,
   * not ten writes. */
  GtkWidget *strip = g_object_get_data (G_OBJECT (button), "band-strip");
  for (GtkWidget *c = gtk_widget_get_first_child (strip); c;
       c = gtk_widget_get_next_sibling (c)) {
    GtkWidget *scale = g_object_get_data (G_OBJECT (c), "scale");
    if (scale) {
      g_signal_handlers_block_by_func (scale, on_eq_band_changed, self);
      gtk_range_set_value (GTK_RANGE (scale), 0.0);
      g_signal_handlers_unblock_by_func (scale, on_eq_band_changed, self);
    }
  }
}

/* A ten-band graphic EQ: an enable switch, a reset, and one vertical
 * -12..+12 dB slider per band, labelled with the band's centre frequency. */
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
  gtk_box_append (GTK_BOX (head), reset);
  gtk_box_append (GTK_BOX (box), head);

  /* One vertical slider per band. */
  GtkWidget *strip = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_set_homogeneous (GTK_BOX (strip), TRUE);
  gtk_widget_set_margin_start (strip, 12);
  gtk_widget_set_margin_end (strip, 12);
  gtk_widget_set_margin_bottom (strip, 14);

  const gdouble *gains = spotifygtk_settings_get_eq_gains (self->settings);
  for (int b = 0; b < SPOTIFYGTK_EQ_BANDS; b++) {
    GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand (col, TRUE);

    GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_VERTICAL,
                                                 -12.0, 12.0, 1.0);
    gtk_range_set_inverted (GTK_RANGE (scale), TRUE);  /* + at the top */
    gtk_range_set_value (GTK_RANGE (scale), gains ? gains[b] : 0.0);
    gtk_scale_set_draw_value (GTK_SCALE (scale), FALSE);
    gtk_widget_set_vexpand (scale, TRUE);
    gtk_widget_set_size_request (scale, -1, 140);
    gtk_scale_add_mark (GTK_SCALE (scale), 0.0, GTK_POS_LEFT, NULL);
    g_object_set_data (G_OBJECT (scale), "band", GUINT_TO_POINTER (b));
    g_signal_connect (scale, "value-changed", G_CALLBACK (on_eq_band_changed), self);
    gtk_box_append (GTK_BOX (col), scale);

    g_autofree gchar *hz = format_freq (spotifygtk_eq_frequencies[b]);
    GtkWidget *lbl = gtk_label_new (hz);
    gtk_widget_add_css_class (lbl, "dim-text");
    gtk_box_append (GTK_BOX (col), lbl);

    g_object_set_data (G_OBJECT (col), "scale", scale);
    gtk_box_append (GTK_BOX (strip), col);
  }

  g_object_set_data (G_OBJECT (reset), "band-strip", strip);
  g_signal_connect (reset, "clicked", G_CALLBACK (on_eq_reset_clicked), self);

  gtk_box_append (GTK_BOX (box), strip);
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
  gtk_box_append (GTK_BOX (audio_group),
                  build_row ("Sample rate",
                             "Output follows the stream (44.1 kHz) today. "
                             "Choosing a rate needs a resampler first.",
                             build_dropdown (rates,
                               spotifygtk_settings_get_sample_rate (self->settings),
                               FALSE)));

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

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), content);
  gtk_box_append (GTK_BOX (self), scroller);
}

SpotifyGtkSettingsPage *
spotifygtk_settings_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_SETTINGS_PAGE, NULL);
}

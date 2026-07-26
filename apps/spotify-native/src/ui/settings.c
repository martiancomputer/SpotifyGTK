/*
 * settings.c — Persisted user preferences.
 */

#include "settings.h"

#include <glib/gstdio.h>
#include <string.h>

#define SETTINGS_GROUP "spotify-native"

struct _SpotifyGtkSettings {
  GObject parent_instance;

  SpotifyGtkTheme      theme;
  SpotifyGtkMediaMode  media_mode;
  SpotifyGtkSampleRate sample_rate;

  gboolean eq_enabled;
  gdouble  eq_gains[SPOTIFYGTK_SETTINGS_EQ_BANDS];  /* dB per band */

  gchar *path;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkSettings, spotifygtk_settings, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
spotifygtk_settings_class_init (SpotifyGtkSettingsClass *klass)
{
  signals[CHANGED] = g_signal_new ("changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void
load (SpotifyGtkSettings *self)
{
  g_autoptr(GKeyFile) kf = g_key_file_new ();

  /* Absent file is the normal first-run case, not an error worth reporting;
   * the defaults set in init() stand. */
  if (!g_key_file_load_from_file (kf, self->path, G_KEY_FILE_NONE, NULL))
    return;

  self->theme = (SpotifyGtkTheme)
    g_key_file_get_integer (kf, SETTINGS_GROUP, "theme", NULL);
  self->media_mode = (SpotifyGtkMediaMode)
    g_key_file_get_integer (kf, SETTINGS_GROUP, "media-mode", NULL);
  self->sample_rate = (SpotifyGtkSampleRate)
    g_key_file_get_integer (kf, SETTINGS_GROUP, "sample-rate", NULL);

  self->eq_enabled = g_key_file_get_boolean (kf, SETTINGS_GROUP, "eq-enabled", NULL);
  gsize n = 0;
  g_autofree gdouble *g = g_key_file_get_double_list (kf, SETTINGS_GROUP,
                                                      "eq-gains", &n, NULL);
  for (gsize i = 0; g && i < n && i < SPOTIFYGTK_SETTINGS_EQ_BANDS; i++)
    self->eq_gains[i] = CLAMP (g[i], -12.0, 12.0);

  /* A hand-edited or truncated file must not put the UI into a state its
   * own controls cannot represent, so anything out of range falls back. */
  if (self->theme > SPOTIFYGTK_THEME_MILK)
    self->theme = SPOTIFYGTK_THEME_DARK;
  if (self->media_mode > SPOTIFYGTK_MEDIA_FULL)
    self->media_mode = SPOTIFYGTK_MEDIA_FULL;
  if (self->sample_rate > SPOTIFYGTK_SAMPLE_RATE_96000)
    self->sample_rate = SPOTIFYGTK_SAMPLE_RATE_DEFAULT;
}

static void
save (SpotifyGtkSettings *self)
{
  g_autoptr(GKeyFile) kf = g_key_file_new ();

  g_key_file_set_integer (kf, SETTINGS_GROUP, "theme", self->theme);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "media-mode", self->media_mode);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "sample-rate", self->sample_rate);
  g_key_file_set_boolean (kf, SETTINGS_GROUP, "eq-enabled", self->eq_enabled);
  g_key_file_set_double_list (kf, SETTINGS_GROUP, "eq-gains", self->eq_gains,
                              SPOTIFYGTK_SETTINGS_EQ_BANDS);

  g_autofree gchar *dir = g_path_get_dirname (self->path);
  g_mkdir_with_parents (dir, 0700);

  g_autoptr(GError) error = NULL;
  if (!g_key_file_save_to_file (kf, self->path, &error))
    g_warning ("settings: could not save %s: %s", self->path, error->message);
}

static void
spotifygtk_settings_init (SpotifyGtkSettings *self)
{
  self->theme       = SPOTIFYGTK_THEME_DARK;
  self->media_mode  = SPOTIFYGTK_MEDIA_FULL;
  self->sample_rate = SPOTIFYGTK_SAMPLE_RATE_DEFAULT;

  self->path = g_build_filename (g_get_user_config_dir (),
                                 "spotify-native", "settings.ini", NULL);
  load (self);
}

SpotifyGtkSettings *
spotifygtk_settings_get_default (void)
{
  static SpotifyGtkSettings *instance = NULL;

  if (!instance)
    instance = g_object_new (SPOTIFYGTK_TYPE_SETTINGS, NULL);

  return instance;
}

/* Each setter stores, persists, then announces — in that order, so a handler
 * reading the value back during "changed" sees the new one. */
#define DEFINE_SETTING(name, type, field, max)                                \
  type                                                                        \
  spotifygtk_settings_get_##name (SpotifyGtkSettings *self)                   \
  {                                                                           \
    g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), 0);                  \
    return self->field;                                                       \
  }                                                                           \
                                                                              \
  void                                                                        \
  spotifygtk_settings_set_##name (SpotifyGtkSettings *self, type value)       \
  {                                                                           \
    g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));                         \
    if (value > (max) || self->field == value)                                \
      return;                                                                 \
    self->field = value;                                                      \
    save (self);                                                              \
    g_signal_emit (self, signals[CHANGED], 0);                                \
  }

DEFINE_SETTING (theme,       SpotifyGtkTheme,      theme,       SPOTIFYGTK_THEME_MILK)
DEFINE_SETTING (media_mode,  SpotifyGtkMediaMode,  media_mode,  SPOTIFYGTK_MEDIA_FULL)
DEFINE_SETTING (sample_rate, SpotifyGtkSampleRate, sample_rate, SPOTIFYGTK_SAMPLE_RATE_96000)

const gdouble *
spotifygtk_settings_get_eq_gains (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), NULL);
  return self->eq_gains;
}

gboolean
spotifygtk_settings_get_eq_enabled (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), FALSE);
  return self->eq_enabled;
}

void
spotifygtk_settings_set_eq_band (SpotifyGtkSettings *self, guint band, gdouble gain_db)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  if (band >= SPOTIFYGTK_SETTINGS_EQ_BANDS)
    return;
  gdouble v = CLAMP (gain_db, -12.0, 12.0);
  if (self->eq_gains[band] == v)
    return;
  self->eq_gains[band] = v;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

void
spotifygtk_settings_set_eq_enabled (SpotifyGtkSettings *self, gboolean enabled)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  if (self->eq_enabled == enabled)
    return;
  self->eq_enabled = enabled;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

void
spotifygtk_settings_reset_eq (SpotifyGtkSettings *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  memset (self->eq_gains, 0, sizeof self->eq_gains);
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

gint
spotifygtk_settings_sample_rate_hz (SpotifyGtkSampleRate rate)
{
  switch (rate) {
    case SPOTIFYGTK_SAMPLE_RATE_44100: return 44100;
    case SPOTIFYGTK_SAMPLE_RATE_48000: return 48000;
    case SPOTIFYGTK_SAMPLE_RATE_96000: return 96000;
    default:                           return 0;   /* follow the stream */
  }
}

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
  SpotifyGtkSampleFormat sample_format;
  SpotifyGtkResamplerMode resampler_mode;
  SpotifyGtkRenderer   renderer;

  gboolean eq_enabled;
  gboolean aggressive_filtering;
  gboolean compact_mode;
  gboolean page_crossfade;
  gboolean caching_enabled;
  gboolean online_lyrics;
  gboolean local_files_enabled;
  guint    lyrics_font_size;
  guint    scroll_smoothness;
  gboolean shuffle;
  guint    repeat;
  gdouble  eq_gains[SPOTIFYGTK_SETTINGS_EQ_BANDS];  /* dB per band */

  gchar *path;

  GPtrArray *pins;   /* SpotifyGtkPin* */
  GPtrArray *local_directories; /* absolute UTF-8 paths, gchar* */
  GHashTable *unavailable;   /* track uri -> seen; the server had no file */
};

static void
pin_free (gpointer data)
{
  SpotifyGtkPin *p = data;
  g_free (p->uri);
  g_free (p->name);
  g_free (p->type);
  g_free (p->cover_id);
  g_free (p);
}

G_DEFINE_FINAL_TYPE (SpotifyGtkSettings, spotifygtk_settings, G_TYPE_OBJECT)

enum { CHANGED, LOCAL_FILES_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
spotifygtk_settings_finalize (GObject *object)
{
  SpotifyGtkSettings *self = SPOTIFYGTK_SETTINGS (object);
  g_clear_pointer (&self->path, g_free);
  g_clear_pointer (&self->pins, g_ptr_array_unref);
  g_clear_pointer (&self->local_directories, g_ptr_array_unref);
  g_clear_pointer (&self->unavailable, g_hash_table_unref);
  G_OBJECT_CLASS (spotifygtk_settings_parent_class)->finalize (object);
}

static void
spotifygtk_settings_class_init (SpotifyGtkSettingsClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = spotifygtk_settings_finalize;
  signals[CHANGED] = g_signal_new ("changed",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
  signals[LOCAL_FILES_CHANGED] = g_signal_new ("local-files-changed",
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
  self->sample_format = (SpotifyGtkSampleFormat)
    g_key_file_get_integer (kf, SETTINGS_GROUP, "sample-format", NULL);
  self->resampler_mode = (SpotifyGtkResamplerMode)
    g_key_file_get_integer (kf, SETTINGS_GROUP, "resampler-mode", NULL);
  self->renderer = (SpotifyGtkRenderer)
    g_key_file_get_integer (kf, SETTINGS_GROUP, "renderer", NULL);

  self->eq_enabled = g_key_file_get_boolean (kf, SETTINGS_GROUP, "eq-enabled", NULL);
  self->aggressive_filtering =
    g_key_file_get_boolean (kf, SETTINGS_GROUP, "aggressive-filtering", NULL);
  if (g_key_file_has_key (kf, SETTINGS_GROUP, "compact-mode", NULL))
    self->compact_mode =
      g_key_file_get_boolean (kf, SETTINGS_GROUP, "compact-mode", NULL);
  if (g_key_file_has_key (kf, SETTINGS_GROUP, "page-crossfade", NULL))
    self->page_crossfade =
      g_key_file_get_boolean (kf, SETTINGS_GROUP, "page-crossfade", NULL);
  if (g_key_file_has_key (kf, SETTINGS_GROUP, "caching-enabled", NULL))
    self->caching_enabled =
      g_key_file_get_boolean (kf, SETTINGS_GROUP, "caching-enabled", NULL);
  if (g_key_file_has_key (kf, SETTINGS_GROUP, "online-lyrics", NULL))
    self->online_lyrics =
      g_key_file_get_boolean (kf, SETTINGS_GROUP, "online-lyrics", NULL);
  if (g_key_file_has_key (kf, SETTINGS_GROUP, "local-files-enabled", NULL))
    self->local_files_enabled =
      g_key_file_get_boolean (kf, SETTINGS_GROUP, "local-files-enabled", NULL);
  gsize n_local = 0;
  g_auto(GStrv) local_paths = g_key_file_get_string_list (
    kf, SETTINGS_GROUP, "local-directories", &n_local, NULL);
  for (gsize i = 0; local_paths && i < n_local && i < 128; i++) {
    if (!g_path_is_absolute (local_paths[i]) ||
        !g_utf8_validate (local_paths[i], -1, NULL))
      continue;
    g_autofree gchar *canonical = g_canonicalize_filename (local_paths[i], NULL);
    gboolean duplicate = FALSE;
    for (guint j = 0; j < self->local_directories->len; j++)
      duplicate |= g_strcmp0 (canonical,
                              g_ptr_array_index (self->local_directories, j)) == 0;
    if (!duplicate)
      g_ptr_array_add (self->local_directories, g_steal_pointer (&canonical));
  }
  if (g_key_file_has_key (kf, SETTINGS_GROUP, "lyrics-font-size", NULL)) {
    gint size = g_key_file_get_integer (kf, SETTINGS_GROUP,
                                        "lyrics-font-size", NULL);
    if (size == 19 || size == 20 ||
        (size >= 22 && size <= 28 && size % 2 == 0))
      self->lyrics_font_size = (guint) size;
  }
  if (g_key_file_has_key (kf, SETTINGS_GROUP, "scroll-smoothness", NULL))
    self->scroll_smoothness = CLAMP (
      g_key_file_get_integer (kf, SETTINGS_GROUP, "scroll-smoothness", NULL),
      0, 100);
  self->shuffle    = g_key_file_get_boolean (kf, SETTINGS_GROUP, "shuffle", NULL);
  self->repeat     = (guint) g_key_file_get_integer (kf, SETTINGS_GROUP, "repeat", NULL);
  gsize n = 0;
  g_autofree gdouble *g = g_key_file_get_double_list (kf, SETTINGS_GROUP,
                                                      "eq-gains", &n, NULL);
  for (gsize i = 0; g && i < n && i < SPOTIFYGTK_SETTINGS_EQ_BANDS; i++)
    self->eq_gains[i] = CLAMP (g[i], -12.0, 12.0);

  /* Pins. The three lists are written together, but a hand-edited file may
   * disagree, so this walks the shortest and drops the rest rather than
   * indexing off the end of one of them. */
  gsize nun = 0;
  g_auto(GStrv) unavail = g_key_file_get_string_list (kf, SETTINGS_GROUP,
                                                     "unavailable-uris", &nun, NULL);
  for (gsize i = 0; unavail && i < nun; i++)
    if (unavail[i] && *unavail[i])
      g_hash_table_add (self->unavailable, g_strdup (unavail[i]));

  gsize nu = 0, nn = 0, nt = 0, nc = 0;
  g_auto(GStrv) uris  = g_key_file_get_string_list (kf, SETTINGS_GROUP, "pinned-uris",  &nu, NULL);
  g_auto(GStrv) names = g_key_file_get_string_list (kf, SETTINGS_GROUP, "pinned-names", &nn, NULL);
  g_auto(GStrv) types = g_key_file_get_string_list (kf, SETTINGS_GROUP, "pinned-types", &nt, NULL);
  g_auto(GStrv) covers = g_key_file_get_string_list (kf, SETTINGS_GROUP, "pinned-covers", &nc, NULL);
  for (gsize i = 0; uris && i < nu; i++) {
    if (!uris[i] || !*uris[i])
      continue;
    SpotifyGtkPin *p = g_new0 (SpotifyGtkPin, 1);
    p->uri  = g_strdup (uris[i]);
    p->name = g_strdup ((names && i < nn) ? names[i] : uris[i]);
    p->type = g_strdup ((types && i < nt) ? types[i] : "");
    /* Empty means "pinned before covers were stored"; the row shows its
     * placeholder rather than asking the loader for nothing. */
    if (covers && i < nc && covers[i] && *covers[i])
      p->cover_id = g_strdup (covers[i]);
    g_ptr_array_add (self->pins, p);
  }

  /* A hand-edited or truncated file must not put the UI into a state its
   * own controls cannot represent, so anything out of range falls back. */
  if (self->theme > SPOTIFYGTK_THEME_DARK_PLUS)
    self->theme = SPOTIFYGTK_THEME_DARK;
  if (self->media_mode > SPOTIFYGTK_MEDIA_NONE)
    self->media_mode = SPOTIFYGTK_MEDIA_FULL;
  if (self->sample_rate > SPOTIFYGTK_SAMPLE_RATE_384000)
    self->sample_rate = SPOTIFYGTK_SAMPLE_RATE_DEFAULT;
  if (self->sample_format > SPOTIFYGTK_SAMPLE_FORMAT_32)
    self->sample_format = SPOTIFYGTK_SAMPLE_FORMAT_16;
  if (self->resampler_mode > SPOTIFYGTK_RESAMPLER_LINEAR)
    self->resampler_mode = SPOTIFYGTK_RESAMPLER_POLYPHASE;
  if (self->renderer > SPOTIFYGTK_RENDERER_CAIRO)
    self->renderer = SPOTIFYGTK_RENDERER_AUTOMATIC;
}

static void
save (SpotifyGtkSettings *self)
{
  g_autoptr(GKeyFile) kf = g_key_file_new ();

  g_key_file_set_integer (kf, SETTINGS_GROUP, "theme", self->theme);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "media-mode", self->media_mode);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "sample-rate", self->sample_rate);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "sample-format", self->sample_format);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "resampler-mode", self->resampler_mode);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "renderer", self->renderer);
  g_key_file_set_boolean (kf, SETTINGS_GROUP, "eq-enabled", self->eq_enabled);
  g_key_file_set_boolean (kf, SETTINGS_GROUP, "aggressive-filtering",
                          self->aggressive_filtering);
  g_key_file_set_boolean (kf, SETTINGS_GROUP, "compact-mode",
                          self->compact_mode);
  g_key_file_set_boolean (kf, SETTINGS_GROUP, "page-crossfade",
                          self->page_crossfade);
  g_key_file_set_boolean (kf, SETTINGS_GROUP, "caching-enabled",
                          self->caching_enabled);
  g_key_file_set_boolean (kf, SETTINGS_GROUP, "online-lyrics",
                          self->online_lyrics);
  g_key_file_set_boolean (kf, SETTINGS_GROUP, "local-files-enabled",
                          self->local_files_enabled);
  if (self->local_directories->len > 0)
    g_key_file_set_string_list (kf, SETTINGS_GROUP, "local-directories",
      (const gchar *const *) self->local_directories->pdata,
      self->local_directories->len);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "lyrics-font-size",
                          (gint) self->lyrics_font_size);
  /* Drop the retired concurrency workaround when rewriting an older file. */
  g_key_file_remove_key (kf, SETTINGS_GROUP, "aggressive-media", NULL);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "scroll-smoothness",
                          (gint) self->scroll_smoothness);
  g_key_file_set_boolean (kf, SETTINGS_GROUP, "shuffle", self->shuffle);
  g_key_file_set_integer (kf, SETTINGS_GROUP, "repeat", (gint) self->repeat);
  g_key_file_set_double_list (kf, SETTINGS_GROUP, "eq-gains", self->eq_gains,
                              SPOTIFYGTK_SETTINGS_EQ_BANDS);

  if (self->pins && self->pins->len > 0) {
    g_autofree const gchar **uris  = g_new0 (const gchar *, self->pins->len);
    g_autofree const gchar **names = g_new0 (const gchar *, self->pins->len);
    g_autofree const gchar **types = g_new0 (const gchar *, self->pins->len);
    g_autofree const gchar **covers = g_new0 (const gchar *, self->pins->len);
    for (guint i = 0; i < self->pins->len; i++) {
      const SpotifyGtkPin *p = g_ptr_array_index (self->pins, i);
      uris[i]  = p->uri;
      names[i] = p->name ? p->name : "";
      types[i] = p->type ? p->type : "";
      covers[i] = p->cover_id ? p->cover_id : "";
    }
    g_key_file_set_string_list (kf, SETTINGS_GROUP, "pinned-uris",  uris,  self->pins->len);
    g_key_file_set_string_list (kf, SETTINGS_GROUP, "pinned-names", names, self->pins->len);
    g_key_file_set_string_list (kf, SETTINGS_GROUP, "pinned-types", types, self->pins->len);
    g_key_file_set_string_list (kf, SETTINGS_GROUP, "pinned-covers", covers, self->pins->len);
  } else {
    g_key_file_remove_key (kf, SETTINGS_GROUP, "pinned-uris",  NULL);
    g_key_file_remove_key (kf, SETTINGS_GROUP, "pinned-names", NULL);
    g_key_file_remove_key (kf, SETTINGS_GROUP, "pinned-types", NULL);
    g_key_file_remove_key (kf, SETTINGS_GROUP, "pinned-covers", NULL);
  }

  /*
   * Remembered across runs on purpose: the list metadata does not say whether
   * a track has a file -- the batch response omits the field for entities that
   * play perfectly well, measured at 605 of 4800 -- so the only reliable
   * source is having tried. Throwing that away at exit would mean rediscovering
   * each dead track by stalling on it again.
   */
  if (g_hash_table_size (self->unavailable) > 0) {
    guint n = g_hash_table_size (self->unavailable);
    g_autofree const gchar **list = g_new0 (const gchar *, n + 1);
    GHashTableIter it; gpointer k; guint i = 0;
    g_hash_table_iter_init (&it, self->unavailable);
    while (g_hash_table_iter_next (&it, &k, NULL))
      list[i++] = k;
    g_key_file_set_string_list (kf, SETTINGS_GROUP, "unavailable-uris", list, n);
  }

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
  self->renderer    = SPOTIFYGTK_RENDERER_AUTOMATIC;
  self->caching_enabled = TRUE;
  self->local_files_enabled = TRUE;
  self->compact_mode = TRUE;
  self->page_crossfade = TRUE;
  self->lyrics_font_size = 19;
  self->scroll_smoothness = 50;
  self->pins        = g_ptr_array_new_with_free_func (pin_free);
  self->local_directories = g_ptr_array_new_with_free_func (g_free);
  self->unavailable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

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

const GPtrArray *
spotifygtk_settings_get_local_directories (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), NULL);
  return self->local_directories;
}

gboolean
spotifygtk_settings_get_local_files_enabled (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), FALSE);
  return self->local_files_enabled;
}

void
spotifygtk_settings_set_local_files_enabled (SpotifyGtkSettings *self,
                                             gboolean enabled)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  enabled = !!enabled;
  if (self->local_files_enabled == enabled)
    return;
  self->local_files_enabled = enabled;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
  g_signal_emit (self, signals[LOCAL_FILES_CHANGED], 0);
}

gboolean
spotifygtk_settings_add_local_directory (SpotifyGtkSettings *self,
                                          const gchar *path)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), FALSE);
  if (!path || !g_path_is_absolute (path) ||
      !g_utf8_validate (path, -1, NULL))
    return FALSE;
  g_autofree gchar *canonical = g_canonicalize_filename (path, NULL);
  if (!g_file_test (canonical, G_FILE_TEST_IS_DIR) ||
      self->local_directories->len >= 128)
    return FALSE;
  for (guint i = 0; i < self->local_directories->len; i++)
    if (g_strcmp0 (canonical, g_ptr_array_index (self->local_directories, i)) == 0)
      return FALSE;
  g_ptr_array_add (self->local_directories, g_steal_pointer (&canonical));
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
  g_signal_emit (self, signals[LOCAL_FILES_CHANGED], 0);
  return TRUE;
}

gboolean
spotifygtk_settings_remove_local_directory (SpotifyGtkSettings *self,
                                             const gchar *path)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), FALSE);
  if (!path)
    return FALSE;
  for (guint i = 0; i < self->local_directories->len; i++) {
    if (g_strcmp0 (path, g_ptr_array_index (self->local_directories, i)) != 0)
      continue;
    g_ptr_array_remove_index (self->local_directories, i);
    save (self);
    g_signal_emit (self, signals[CHANGED], 0);
    g_signal_emit (self, signals[LOCAL_FILES_CHANGED], 0);
    return TRUE;
  }
  return FALSE;
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

DEFINE_SETTING (theme,       SpotifyGtkTheme,      theme,       SPOTIFYGTK_THEME_DARK_PLUS)
DEFINE_SETTING (media_mode,  SpotifyGtkMediaMode,  media_mode,  SPOTIFYGTK_MEDIA_NONE)
DEFINE_SETTING (sample_rate, SpotifyGtkSampleRate, sample_rate, SPOTIFYGTK_SAMPLE_RATE_384000)
DEFINE_SETTING (sample_format, SpotifyGtkSampleFormat, sample_format, SPOTIFYGTK_SAMPLE_FORMAT_32)
DEFINE_SETTING (resampler_mode, SpotifyGtkResamplerMode, resampler_mode, SPOTIFYGTK_RESAMPLER_LINEAR)
DEFINE_SETTING (renderer,    SpotifyGtkRenderer,   renderer,    SPOTIFYGTK_RENDERER_CAIRO)

guint
spotifygtk_settings_get_lyrics_font_size (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), 19);
  return self->lyrics_font_size;
}

void
spotifygtk_settings_set_lyrics_font_size (SpotifyGtkSettings *self, guint pixels)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  if (!(pixels == 19 || pixels == 20 ||
        (pixels >= 22 && pixels <= 28 && pixels % 2 == 0)) ||
      self->lyrics_font_size == pixels)
    return;
  self->lyrics_font_size = pixels;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

gboolean
spotifygtk_settings_get_aggressive_filtering (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), FALSE);
  return self->aggressive_filtering;
}

void
spotifygtk_settings_set_aggressive_filtering (SpotifyGtkSettings *self,
                                               gboolean enabled)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  enabled = !!enabled;
  if (self->aggressive_filtering == enabled)
    return;
  self->aggressive_filtering = enabled;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

gboolean
spotifygtk_settings_get_compact_mode (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), TRUE);
  return self->compact_mode;
}

void
spotifygtk_settings_set_compact_mode (SpotifyGtkSettings *self,
                                      gboolean enabled)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  enabled = !!enabled;
  if (self->compact_mode == enabled)
    return;
  self->compact_mode = enabled;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

gboolean
spotifygtk_settings_get_page_crossfade (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), TRUE);
  return self->page_crossfade;
}

void
spotifygtk_settings_set_page_crossfade (SpotifyGtkSettings *self,
                                         gboolean enabled)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  enabled = !!enabled;
  if (self->page_crossfade == enabled)
    return;
  self->page_crossfade = enabled;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

gboolean
spotifygtk_settings_get_caching_enabled (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), TRUE);
  return self->caching_enabled;
}

void
spotifygtk_settings_set_caching_enabled (SpotifyGtkSettings *self,
                                         gboolean            enabled)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  enabled = !!enabled;
  if (self->caching_enabled == enabled)
    return;
  self->caching_enabled = enabled;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

gboolean
spotifygtk_settings_get_online_lyrics (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), FALSE);
  return self->online_lyrics;
}

void
spotifygtk_settings_set_online_lyrics (SpotifyGtkSettings *self,
                                       gboolean enabled)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  enabled = !!enabled;
  if (self->online_lyrics == enabled)
    return;
  self->online_lyrics = enabled;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

guint
spotifygtk_settings_get_scroll_smoothness (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), 50);
  return self->scroll_smoothness;
}

void
spotifygtk_settings_set_scroll_smoothness (SpotifyGtkSettings *self,
                                           guint               value)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  value = MIN (value, 100);
  if (self->scroll_smoothness == value)
    return;
  self->scroll_smoothness = value;
  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

const gchar *
spotifygtk_renderer_backend (SpotifyGtkRenderer renderer)
{
  switch (renderer) {
    case SPOTIFYGTK_RENDERER_VULKAN: return "vulkan";
    case SPOTIFYGTK_RENDERER_OPENGL: return "opengl";
    case SPOTIFYGTK_RENDERER_CAIRO:  return "cairo";
    default:                         return NULL;
  }
}

const gdouble *
spotifygtk_settings_get_eq_gains (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), NULL);
  return self->eq_gains;
}

gboolean
spotifygtk_settings_get_shuffle (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), FALSE);
  return self->shuffle;
}

void
spotifygtk_settings_set_shuffle (SpotifyGtkSettings *self, gboolean on)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  if (self->shuffle == on) return;
  self->shuffle = on;
  save (self);
}

guint
spotifygtk_settings_get_repeat (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), 0);
  return self->repeat;
}

void
spotifygtk_settings_set_repeat (SpotifyGtkSettings *self, guint mode)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  if (self->repeat == mode) return;
  self->repeat = mode;
  save (self);
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
    case SPOTIFYGTK_SAMPLE_RATE_192000: return 192000;
    case SPOTIFYGTK_SAMPLE_RATE_384000: return 384000;
    default:                           return 0;   /* follow the stream */
  }
}

/* ── Unavailable tracks ──────────────────────────────────────────────────
 *
 * Learned by trying to play, because nothing in the list metadata says so
 * reliably. Saved on every addition for the same reason the pins are.
 */
GHashTable *
spotifygtk_settings_get_unavailable (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), NULL);
  return self->unavailable;
}

void
spotifygtk_settings_mark_unavailable (SpotifyGtkSettings *self, const gchar *uri)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  if (!uri || !*uri || g_hash_table_contains (self->unavailable, uri))
    return;
  g_hash_table_add (self->unavailable, g_strdup (uri));
  save (self);
}

/* ── Pins ────────────────────────────────────────────────────────────────
 *
 * Kept in the order they were pinned, which is the order the sidebar shows.
 * Every mutation saves immediately: a pin that survives until the next clean
 * shutdown but not a crash is worse than no pin at all.
 */
GPtrArray *
spotifygtk_settings_get_pins (SpotifyGtkSettings *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), NULL);
  return self->pins;
}

gboolean
spotifygtk_settings_is_pinned (SpotifyGtkSettings *self, const gchar *uri)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_SETTINGS (self), FALSE);
  if (!uri)
    return FALSE;
  for (guint i = 0; i < self->pins->len; i++)
    if (g_strcmp0 (((SpotifyGtkPin *) g_ptr_array_index (self->pins, i))->uri, uri) == 0)
      return TRUE;
  return FALSE;
}

void
spotifygtk_settings_add_pin (SpotifyGtkSettings *self, const gchar *uri,
                             const gchar *name, const gchar *type,
                             const gchar *cover_id)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  if (!uri || !*uri || spotifygtk_settings_is_pinned (self, uri))
    return;

  SpotifyGtkPin *p = g_new0 (SpotifyGtkPin, 1);
  p->uri  = g_strdup (uri);
  p->name = g_strdup (name && *name ? name : uri);
  p->type = g_strdup (type ? type : "");
  p->cover_id = (cover_id && *cover_id) ? g_strdup (cover_id) : NULL;
  g_ptr_array_add (self->pins, p);

  save (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

void
spotifygtk_settings_remove_pin (SpotifyGtkSettings *self, const gchar *uri)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (self));
  if (!uri)
    return;

  for (guint i = 0; i < self->pins->len; i++) {
    if (g_strcmp0 (((SpotifyGtkPin *) g_ptr_array_index (self->pins, i))->uri, uri) == 0) {
      /* remove_index, not remove_index_fast: the order is the order they were
       * pinned in, and that is the order the sidebar shows. */
      g_ptr_array_remove_index (self->pins, i);
      save (self);
      g_signal_emit (self, signals[CHANGED], 0);
      return;
    }
  }
}

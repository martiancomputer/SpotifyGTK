/*
 * settings.h — Persisted user preferences.
 *
 * A process-wide singleton backed by a key file at
 * ~/.config/spotify-native/settings.ini. GSettings would mean shipping and
 * installing a schema before the app can start, which is a poor trade while
 * the option set is still moving; a key file has no install step and is
 * inspectable by hand.
 *
 * Only the options that actually do something today are read by anything.
 * The rest are stored so the UI can round-trip them, and are marked in the
 * settings page as not yet implemented rather than silently ignored.
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
  SPOTIFYGTK_THEME_DARK,
  SPOTIFYGTK_THEME_LIGHT,
  SPOTIFYGTK_THEME_MILK,
} SpotifyGtkTheme;

/*
 * TEXT_ONLY suppresses artwork everywhere — rows, playback bar, Now Playing
 * panel — leaving the placeholder icons. Enforced centrally in
 * cover_loader.c rather than at each display site, so a new artwork
 * consumer cannot forget to honour it.
 */
typedef enum {
  SPOTIFYGTK_MEDIA_TEXT_ONLY,
  SPOTIFYGTK_MEDIA_FULL,
} SpotifyGtkMediaMode;

typedef enum {
  SPOTIFYGTK_SAMPLE_RATE_DEFAULT,
  SPOTIFYGTK_SAMPLE_RATE_44100,
  SPOTIFYGTK_SAMPLE_RATE_48000,
  SPOTIFYGTK_SAMPLE_RATE_96000,
} SpotifyGtkSampleRate;

#define SPOTIFYGTK_TYPE_SETTINGS (spotifygtk_settings_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkSettings, spotifygtk_settings,
                      SPOTIFYGTK, SETTINGS, GObject)

/* Shared instance; loads from disk on first use. */
SpotifyGtkSettings *spotifygtk_settings_get_default (void);

SpotifyGtkMediaMode spotifygtk_settings_get_media_mode (SpotifyGtkSettings *self);
void                spotifygtk_settings_set_media_mode (SpotifyGtkSettings *self,
                                                        SpotifyGtkMediaMode mode);

SpotifyGtkTheme spotifygtk_settings_get_theme (SpotifyGtkSettings *self);
void            spotifygtk_settings_set_theme (SpotifyGtkSettings *self,
                                               SpotifyGtkTheme theme);

SpotifyGtkSampleRate spotifygtk_settings_get_sample_rate (SpotifyGtkSettings *self);
void                 spotifygtk_settings_set_sample_rate (SpotifyGtkSettings *self,
                                                          SpotifyGtkSampleRate rate);

#define SPOTIFYGTK_SETTINGS_EQ_BANDS 15

const gdouble *spotifygtk_settings_get_eq_gains   (SpotifyGtkSettings *self);
/* Shuffle, and repeat as SpotifyGtkRepeatMode. Persisted like the rest: the
 * modes are a standing preference, not per-session state. */
gboolean spotifygtk_settings_get_shuffle (SpotifyGtkSettings *self);
void     spotifygtk_settings_set_shuffle (SpotifyGtkSettings *self, gboolean on);
guint    spotifygtk_settings_get_repeat  (SpotifyGtkSettings *self);
void     spotifygtk_settings_set_repeat  (SpotifyGtkSettings *self, guint mode);

gboolean       spotifygtk_settings_get_eq_enabled (SpotifyGtkSettings *self);
void           spotifygtk_settings_set_eq_band    (SpotifyGtkSettings *self,
                                                   guint band, gdouble gain_db);
void           spotifygtk_settings_set_eq_enabled (SpotifyGtkSettings *self,
                                                   gboolean enabled);
void           spotifygtk_settings_reset_eq       (SpotifyGtkSettings *self);

/* Signal: changed () — emitted after any setter, once the value is stored. */

/* Hz for a SpotifyGtkSampleRate; 0 for DEFAULT, meaning "follow the stream"
 * (no conversion). */
gint spotifygtk_settings_sample_rate_hz (SpotifyGtkSampleRate rate);

/* ── Pinned albums and playlists ──────────────────────────────────────────
 *
 * A pin is a URI, the name to show, and the type line under it. Stored as
 * three parallel lists rather than one delimited string: a name can contain
 * anything, and inventing a separator that album titles are guaranteed not to
 * use is the kind of assumption that breaks on one record in a thousand.
 *
 * Order is the order they were pinned. Pinning something already pinned is a
 * no-op rather than a duplicate.
 */
typedef struct {
  gchar *uri;
  gchar *name;
  gchar *type;   /* "Album" / "Playlist", for the row's second line */
} SpotifyGtkPin;

/* The pins, in order. Owned by the settings object; valid until it changes. */
GPtrArray *spotifygtk_settings_get_pins (SpotifyGtkSettings *self);

gboolean spotifygtk_settings_is_pinned (SpotifyGtkSettings *self,
                                        const gchar *uri);

/* Both persist immediately and emit "changed". Adding something already
 * pinned, or removing something that is not, does nothing. */
void spotifygtk_settings_add_pin (SpotifyGtkSettings *self, const gchar *uri,
                                  const gchar *name, const gchar *type);
void spotifygtk_settings_remove_pin (SpotifyGtkSettings *self, const gchar *uri);

G_END_DECLS

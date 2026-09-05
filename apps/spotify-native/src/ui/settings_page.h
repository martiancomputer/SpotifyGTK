/*
 * settings_page.h — Settings page.
 *
 * Interface, Audio and Performance groups. Most options are stored but not
 * yet acted on; those controls are insensitive and say why. "Previews" is
 * the exception and is fully wired — see cover_loader.c.
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_SETTINGS_PAGE (spotifygtk_settings_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkSettingsPage, spotifygtk_settings_page,
                      SPOTIFYGTK, SETTINGS_PAGE, GtkBox)

SpotifyGtkSettingsPage *spotifygtk_settings_page_new (void);

/* Show who is signed in, above the button that signs them out. */
void spotifygtk_settings_page_set_account (SpotifyGtkSettingsPage *self,
                                           const gchar            *username);
void spotifygtk_settings_page_set_account_profile (
  SpotifyGtkSettingsPage *self, const gchar *display_name,
  const gchar *canonical_id, const gchar *product, const gchar *avatar_id);

/* Signal: "log-out" -- the user asked to sign out; the window performs it. */

G_END_DECLS

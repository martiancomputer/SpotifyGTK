/*
 * window.h — Main application window matching the mockup design.
 *
 * Layout:
 * - Header bar (top)
 * - Sidebar (left, 270px)
 * - Main content (center)
 * - Now Playing panel (right, 300px)
 * - Playback bar (bottom, 80px)
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_NATIVE_WINDOW (spotifygtk_native_window_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkNativeWindow, spotifygtk_native_window,
                      SPOTIFYGTK, NATIVE_WINDOW, GtkApplicationWindow)

SpotifyGtkNativeWindow *spotifygtk_native_window_new (GtkApplication *app);

/* Navigation */
void spotifygtk_native_window_navigate_to (SpotifyGtkNativeWindow *self,
                                           const gchar *page_name);

/* Playback state */
void spotifygtk_native_window_set_playing (SpotifyGtkNativeWindow *self,
                                           gboolean is_playing);
void spotifygtk_native_window_set_track_info (SpotifyGtkNativeWindow *self,
                                              const gchar *track_name,
                                              const gchar *artist,
                                              const gchar *album,
                                              const gchar *album_art_url);
void spotifygtk_native_window_set_progress (SpotifyGtkNativeWindow *self,
                                            gint64 position_ms,
                                            gint64 duration_ms);

/* Queue panel */
void spotifygtk_native_window_set_queue_expanded (SpotifyGtkNativeWindow *self,
                                                  gboolean expanded);

G_END_DECLS

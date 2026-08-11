/*
 * context_page.h — A generic "resolved context" page (album or artist).
 *
 * "Go to Album" and "Go to Artist" both end at the same place: a URI handed
 * to /context-resolve, which comes back as an ordered track list — exactly
 * what the search and liked-songs pages already render. Rather than two
 * near-identical pages, this one loads any spotify:album:<id> or
 * spotify:artist:<id> (or, in principle, playlist) URI and shows its tracks,
 * with a header naming what was opened.
 *
 * Artist note: /context-resolve on an artist URI returns a *playback context*
 * for that artist, not a curated discography — the same caveat search carries.
 * It is still the artist's tracks, which is what "Go to Artist" promises here.
 */

#pragma once

#include <adwaita.h>

#include "spotify/session.h"
#include "track_list.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_CONTEXT_PAGE (spotifygtk_context_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkContextPage, spotifygtk_context_page,
                      SPOTIFYGTK, CONTEXT_PAGE, GtkBox)

SpotifyGtkContextPage *spotifygtk_context_page_new (void);

void spotifygtk_context_page_set_session (SpotifyGtkContextPage *self,
                                          SpotifyNativeSession  *session);

/* Load `uri` and render its tracks. `title` is the album/artist name shown
 * large; `kind` is the small label above it ("Album", "Artist"). Re-loading
 * the same URI that is already shown is a no-op. */
/*
 * The header's one action: saving an album, or dropping a playlist from the
 * library. The page only ever shows one of the two, so it is one button whose
 * label the window sets, and the window owns what the click means -- it is
 * the thing that holds the session and the library state.
 */
typedef void (*SpotifyGtkContextActionFunc) (const gchar *uri,
                                             const gchar *kind,
                                             gpointer     user_data);

void spotifygtk_context_page_set_action_handler (SpotifyGtkContextPage      *self,
                                                 SpotifyGtkContextActionFunc fn,
                                                 gpointer                    user_data);

/* Label, visibility and weight of that button. `destructive` turns it red on
 * hover -- at rest it stays ordinary, because a page that shouts every time it
 * opens teaches people to ignore it. Hidden when there is no action, so nothing
 * has to be inferred from a stale label. */
void spotifygtk_context_page_set_action (SpotifyGtkContextPage *self,
                                         const gchar *label, gboolean visible,
                                         gboolean destructive);

void spotifygtk_context_page_load (SpotifyGtkContextPage *self,
                                   const gchar           *uri,
                                   const gchar           *title,
                                   const gchar           *kind);

/* Inner track list, so the window wires play-context and the row context menu
 * to it exactly as it does for search and liked songs. */
SpotifyGtkTrackList *spotifygtk_context_page_get_list (SpotifyGtkContextPage *self);

/* Mirror of the other pages, so the playing indicator follows across here. */
void spotifygtk_context_page_set_playing_uri (SpotifyGtkContextPage *self,
                                              const gchar *uri,
                                              gboolean playing);

G_END_DECLS

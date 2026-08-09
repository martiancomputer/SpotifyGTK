/*
 * artist_page.h — Artist page, backed by the native session.
 *
 * One scrolling page rather than the flat track list an artist used to share
 * with albums, laid out as:
 *
 *   Artist / name / year
 *   a hero panel carrying the artist's art
 *   their most-played tracks
 *   Albums, EPs and Singles -- sortable, as inline cards
 *
 * All of it comes from a single /context-resolve of the artist URI. The native
 * stack has no discography endpoint, so the releases are the distinct albums
 * present in that track list, grouped here -- the same approach Home, Search
 * and Library take, and the same reason: real data grouped from what a resolve
 * already returns, rather than invented.
 *
 * The sections are the shared widgets in inline mode, so rows and cards behave
 * exactly as they do everywhere else -- context menu, hearts, play context --
 * without either of them scrolling independently of the page.
 */

#pragma once

#include <adwaita.h>

#include "spotify/session.h"
#include "track_list.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_ARTIST_PAGE (spotifygtk_artist_page_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkArtistPage, spotifygtk_artist_page,
                      SPOTIFYGTK, ARTIST_PAGE, GtkBox)

SpotifyGtkArtistPage *spotifygtk_artist_page_new (void);

/*
 * Called for every track list this page creates.
 *
 * The releases are expanded in place, so the page builds a list per release
 * after the window has finished wiring the ones it owns. Without this those
 * rows would play but have no context menu, no like, and no play-context --
 * the same rows behaving differently depending which section they are in.
 */
typedef void (*SpotifyGtkArtistListWireFunc) (SpotifyGtkTrackList *list,
                                              gpointer user_data);

void spotifygtk_artist_page_set_list_wire (SpotifyGtkArtistPage *self,
                                           SpotifyGtkArtistListWireFunc fn,
                                           gpointer user_data);

void spotifygtk_artist_page_set_session (SpotifyGtkArtistPage *self,
                                         SpotifyNativeSession *session);

/* Load and show `artist_uri` (spotify:artist:<id>). `name` is what the row
 * that navigated here already knew, shown until the resolve confirms it. */
void spotifygtk_artist_page_show (SpotifyGtkArtistPage *self,
                                  const gchar *artist_uri,
                                  const gchar *name);

/* The top-tracks list, for play-context and the row context menu -- see
 * spotifygtk_context_page_get_list(). */
SpotifyGtkTrackList *spotifygtk_artist_page_get_list (SpotifyGtkArtistPage *self);

void spotifygtk_artist_page_set_playing_uri (SpotifyGtkArtistPage *self,
                                             const gchar *uri,
                                             gboolean playing);

/* Signals:
 * - track-activated  (SpotifyNativeTrack *track)
 */

G_END_DECLS

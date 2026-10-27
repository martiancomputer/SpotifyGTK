#pragma once
#include "session.h"

GBytes *spotifygtk_catalog_tracks_pack (GPtrArray *tracks);
GPtrArray *spotifygtk_catalog_tracks_unpack (GBytes *bytes);
GBytes *spotifygtk_catalog_releases_pack (GPtrArray *releases);
GPtrArray *spotifygtk_catalog_releases_unpack (GBytes *bytes);
/* Saved-album cards only: no per-album track lists or decoded artwork. Dates
 * use the library's existing integer timestamp map. */
GBytes *spotifygtk_catalog_library_pack (GPtrArray *releases, GHashTable *dates);
GPtrArray *spotifygtk_catalog_library_unpack (GBytes *bytes, GHashTable **dates);

/* Filter an owning track snapshot without changing its order. NULL membership
 * is an incomplete read; an empty non-NULL set is authoritative. */
guint spotifygtk_catalog_tracks_filter (GPtrArray *tracks, GHashTable *membership);

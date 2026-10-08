#pragma once
#include "session.h"

GBytes *spotifygtk_catalog_tracks_pack (GPtrArray *tracks);
GPtrArray *spotifygtk_catalog_tracks_unpack (GBytes *bytes);
GBytes *spotifygtk_catalog_releases_pack (GPtrArray *releases);
GPtrArray *spotifygtk_catalog_releases_unpack (GBytes *bytes);

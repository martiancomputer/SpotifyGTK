/* Local media index. UI access is on the GTK thread; scans produce immutable
 * snapshots on workers and swap them only on the GTK thread. */
#pragma once

#include <gio/gio.h>
#include "spotify/session.h"

G_BEGIN_DECLS

typedef struct _SpotifyGtkLocalSnapshot SpotifyGtkLocalSnapshot;

typedef struct {
  SpotifyNativeTrack *display; /* owned; freed after every borrowed GTK model */
  gchar *path;                 /* owned native filesystem path */
  gchar *album_artist;
  gchar *codec;                /* owned, short FFmpeg codec name */
  gchar *art_path;             /* owned compressed image path, if present */
  gint track_number;
  gint disc_number;
  gint sample_rate;
  gint channels;
  gint source_bits;
  guint64 file_size;
  gint64 mtime_us;
} SpotifyGtkLocalTrack;

typedef struct {
  gchar *uri;
  gchar *title;
  gchar *artist;
  gchar *cover_id;
  gint year;
  gint64 added_at;
  GPtrArray *tracks; /* borrowed SpotifyGtkLocalTrack*, snapshot owns them */
} SpotifyGtkLocalAlbum;

/* Transfer full: each caller must unref. All returned album/track pointers are
 * borrowed from the snapshot and valid only while that reference is held. */
SpotifyGtkLocalSnapshot *spotifygtk_local_catalog_ref_snapshot (void);
SpotifyGtkLocalSnapshot *spotifygtk_local_snapshot_ref (SpotifyGtkLocalSnapshot *snapshot);
void spotifygtk_local_snapshot_unref (SpotifyGtkLocalSnapshot *snapshot);
const GPtrArray *spotifygtk_local_snapshot_albums (SpotifyGtkLocalSnapshot *snapshot);
const SpotifyGtkLocalAlbum *spotifygtk_local_snapshot_find_album (
  SpotifyGtkLocalSnapshot *snapshot, const gchar *uri);

/* Duplicated paths are independent of scan replacement and safe to use in an
 * audio/artwork worker. NULL means the URI was removed or is not local. */
gchar *spotifygtk_local_catalog_dup_track_path (const gchar *uri);
gchar *spotifygtk_local_catalog_dup_art_path (const gchar *cover_id);

#define SPOTIFYGTK_TYPE_LOCAL_CATALOG (spotifygtk_local_catalog_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyGtkLocalCatalog, spotifygtk_local_catalog,
                      SPOTIFYGTK, LOCAL_CATALOG, GObject)

/* Borrowed process singleton. Starts a bounded background scan on demand and
 * emits "changed" on the GTK thread after an immutable snapshot swap. */
SpotifyGtkLocalCatalog *spotifygtk_local_catalog_get_default (void);
void spotifygtk_local_catalog_refresh (SpotifyGtkLocalCatalog *self);

G_END_DECLS

/* Incremental local decode into the process-wide PCM sink. The path is an
 * owned/copy-stable argument for the duration of this worker call. */
#pragma once

#include "../native_engine.h"

gboolean spotifygtk_local_playback_run (const gchar *path,
                                        GCancellable *cancellable,
                                        SpotifyNativeEngineProgressFunc progress,
                                        gpointer progress_data,
                                        SpotifyNativeEngineControl *control);

/*
 * log_file.h — mirror every g_log message to a file beside the executable.
 *
 * Exists because the useful part of a bug report is the *start* of a session
 * (sign-in, AP handshake, format negotiation) and that is exactly what has
 * already scrolled out of a console window by the time anything goes wrong.
 * Asking someone to "paste your console output" reliably loses it.
 *
 * Console output is unaffected: this adds a destination, it does not replace
 * one.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * Installs the writer and opens the log. Call once, as early in main() as
 * possible -- anything logged before this lands on the console only.
 *
 * Never fails in a way the caller must handle: if no writable location can be
 * found, logging carries on to the console alone.
 */
void spotifygtk_log_file_init (void);

/* Flushes and closes. Safe to call without a prior init, and safe to call
 * twice. */
void spotifygtk_log_file_shutdown (void);

/* Absolute path of the active log, or NULL if none is open. Owned by the
 * module; do not free. */
const gchar *spotifygtk_log_file_path (void);

G_END_DECLS

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

/* Forward declaration keeps this small runtime header usable by the
 * headless protocol tests without forcing every consumer to include libsoup. */
typedef struct _SoupSession SoupSession;

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

/* Set GIO/GdkPixbuf's module and schema paths relative to a portable bundle
 * before GTK or libsoup initializes them.  Existing environment overrides are
 * respected, so this remains useful for development diagnostics. */
void spotifygtk_runtime_init (void);

/* Attach the CA database shipped next to a portable Windows build to a
 * libsoup session.  On Linux and on Windows builds without a bundled CA file
 * this is a no-op, preserving GIO's normal system trust database. */
void spotifygtk_soup_session_configure_tls (SoupSession *session);

G_END_DECLS

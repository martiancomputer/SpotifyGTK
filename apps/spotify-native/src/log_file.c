/*
 * log_file.c — see log_file.h.
 */

#include "log_file.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <stdio.h>

#ifdef G_OS_WIN32
#include <windows.h>
#endif

static GMutex   log_mutex;      /* the writer runs on whatever thread logged */
static FILE    *log_stream;
static gchar   *log_path;

static GTlsDatabase *portable_tls_database;
static gsize         portable_tls_database_once;

/*
 * Directory holding the running executable.
 *
 * Deliberately not g_get_current_dir(): a portable build is launched by
 * double-clicking, and the working directory then is wherever the shell felt
 * like -- frequently C:\Windows\system32 -- which is neither writable nor
 * anywhere the user would think to look.
 */
static gchar *
executable_dir (void)
{
#ifdef G_OS_WIN32
  wchar_t buf[MAX_PATH];
  DWORD len = GetModuleFileNameW (NULL, buf, G_N_ELEMENTS (buf));
  if (len == 0 || len >= G_N_ELEMENTS (buf))
    return NULL;
  g_autofree gchar *exe = g_utf16_to_utf8 ((const gunichar2 *) buf, -1, NULL, NULL, NULL);
  return exe ? g_path_get_dirname (exe) : NULL;
#else
  g_autofree gchar *exe = g_file_read_link ("/proc/self/exe", NULL);
  return exe ? g_path_get_dirname (exe) : NULL;
#endif
}

void
spotifygtk_runtime_init (void)
{
#ifdef G_OS_WIN32
  /* GIO and GdkPixbuf normally derive these paths from the MSYS2 prefix that
   * built them.  A portable bundle has no such prefix, so point the loaders
   * and schemas at the copy beside the executable before GTK initializes. */
  g_autofree gchar *directory = executable_dir ();
  if (!directory)
    return;

  if (!g_getenv ("GIO_MODULE_DIR")) {
    g_autofree gchar *modules =
      g_build_filename (directory, "lib", "gio", "modules", NULL);
    if (g_file_test (modules, G_FILE_TEST_IS_DIR))
      g_setenv ("GIO_MODULE_DIR", modules, FALSE);
  }

  if (!g_getenv ("GSETTINGS_SCHEMA_DIR")) {
    g_autofree gchar *schemas =
      g_build_filename (directory, "share", "glib-2.0", "schemas", NULL);
    if (g_file_test (schemas, G_FILE_TEST_IS_DIR))
      g_setenv ("GSETTINGS_SCHEMA_DIR", schemas, FALSE);
  }

  if (!g_getenv ("GDK_PIXBUF_MODULE_FILE")) {
    g_autofree gchar *loaders =
      g_build_filename (directory, "lib", "gdk-pixbuf-2.0", "2.10.0",
                        "loaders.cache", NULL);
    if (g_file_test (loaders, G_FILE_TEST_IS_REGULAR))
      g_setenv ("GDK_PIXBUF_MODULE_FILE", loaders, FALSE);
  }
#endif
}

static gchar *
portable_ca_bundle_path (void)
{
  const gchar *override = g_getenv ("SPOTIFYGTK_CA_BUNDLE");
  if (override && *override)
    return g_strdup (override);

#ifdef G_OS_WIN32
  g_autofree gchar *directory = executable_dir ();
  if (!directory)
    return NULL;

  g_autofree gchar *bundle =
    g_build_filename (directory, "etc", "ssl", "certs", "ca-bundle.crt", NULL);
  if (g_file_test (bundle, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer (&bundle);

  /* Accept the alternate MSYS2 name for hand-built bundles.  The official
   * packaging script ships ca-bundle.crt, but this makes diagnosis and local
   * packaging less surprising. */
  bundle = g_build_filename (directory, "etc", "ssl", "certs",
                             "ca-bundle.trust.crt", NULL);
  if (g_file_test (bundle, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer (&bundle);
#endif

  return NULL;
}

static GTlsDatabase *
portable_tls_database_get (void)
{
  if (g_once_init_enter (&portable_tls_database_once)) {
    g_autofree gchar *path = portable_ca_bundle_path ();
    if (path) {
      g_autoptr(GError) error = NULL;
      portable_tls_database = G_TLS_DATABASE (g_tls_file_database_new (path, &error));
      if (portable_tls_database) {
        g_message ("tls: using bundled CA database %s", path);
      } else {
        g_warning ("tls: could not load CA database %s: %s; falling back to the platform default",
                   path, error ? error->message : "unknown error");
      }
    }
    if (!portable_tls_database && !path)
      g_message ("tls: no bundled CA database; using the platform default");
    g_once_init_leave (&portable_tls_database_once, 1);
  }

  return portable_tls_database;
}

void
spotifygtk_soup_session_configure_tls (SoupSession *session)
{
  g_return_if_fail (SOUP_IS_SESSION (session));

  spotifygtk_runtime_init ();

  GTlsDatabase *database = portable_tls_database_get ();
  if (database)
    g_object_set (session, "tls-database", database, NULL);
}

static const gchar *
level_name (GLogLevelFlags level)
{
  switch (level & G_LOG_LEVEL_MASK) {
    case G_LOG_LEVEL_ERROR:    return "ERROR";
    case G_LOG_LEVEL_CRITICAL: return "CRITICAL";
    case G_LOG_LEVEL_WARNING:  return "WARNING";
    case G_LOG_LEVEL_MESSAGE:  return "Message";
    case G_LOG_LEVEL_INFO:     return "Info";
    case G_LOG_LEVEL_DEBUG:    return "Debug";
    default:                   return "Log";
  }
}

static const gchar *
field_value (const GLogField *fields, gsize n_fields, const gchar *key)
{
  for (gsize i = 0; i < n_fields; i++) {
    if (g_strcmp0 (fields[i].key, key) == 0) {
      /* length -1 marks a NUL-terminated string; anything else is binary and
       * has no business in a text log. */
      if (fields[i].length == -1)
        return (const gchar *) fields[i].value;
      return NULL;
    }
  }
  return NULL;
}

static GLogWriterOutput
log_writer (GLogLevelFlags level, const GLogField *fields, gsize n_fields,
            gpointer user_data)
{
  const gchar *domain = field_value (fields, n_fields, "GLIB_DOMAIN");

  /* Mirror the console's filtering rather than recording everything GLib
   * emits. The debug stream is almost entirely dconf and GIO chatter that the
   * console already hides -- 18 KB in the first eight seconds, against maybe a
   * dozen lines that mean anything -- and it would bury exactly what a bug
   * report is for. G_MESSAGES_DEBUG still widens both destinations together. */
  if (g_log_writer_default_would_drop (level, domain))
    return g_log_writer_default (level, fields, n_fields, user_data);

  g_mutex_lock (&log_mutex);
  if (log_stream) {
    const gchar *message = field_value (fields, n_fields, "MESSAGE");

    g_autoptr(GDateTime) now = g_date_time_new_now_local ();
    g_autofree gchar *stamp = g_date_time_format (now, "%H:%M:%S");

    fprintf (log_stream, "%s.%03d %-8s %s%s%s\n",
             stamp, g_date_time_get_microsecond (now) / 1000,
             level_name (level),
             domain ? domain : "", domain ? ": " : "",
             message ? message : "(no message)");

    /* Flushed per line rather than buffered: the log is worth having precisely
     * when the process dies unexpectedly, and a buffered tail is lost exactly
     * then -- taking the last few lines before the crash, which are the ones
     * that matter. */
    fflush (log_stream);
  }
  g_mutex_unlock (&log_mutex);

  /* Console output is unchanged; this is an additional destination. */
  return g_log_writer_default (level, fields, n_fields, user_data);
}

void
spotifygtk_log_file_init (void)
{
  spotifygtk_runtime_init ();

  if (log_stream)
    return;

  g_mutex_init (&log_mutex);

  /* Beside the executable first -- that is the whole point, a portable build
   * should drop its log where the user already is. An installed build lives
   * somewhere read-only, so fall back rather than losing the log entirely.
   *
   * Writability is settled by opening the file rather than by g_access(W_OK):
   * the constant is not portable between glibc and mingw, and a permission bit
   * is in any case a weaker claim than a successful open. */
  g_autofree gchar *dir = executable_dir ();
  g_autofree gchar *path = NULL;

  for (int attempt = 0; attempt < 2 && !log_stream; attempt++) {
    if (attempt == 1) {
      g_clear_pointer (&dir, g_free);
      dir = g_build_filename (g_get_user_state_dir (), "spotify-native", NULL);
      if (g_mkdir_with_parents (dir, 0700) != 0)
        break;
    }
    if (!dir)
      continue;

    g_clear_pointer (&path, g_free);
    path = g_build_filename (dir, "spotify-native.log", NULL);

    /* Keep one generation. A tester who hits a crash, restarts, and only then
     * goes looking would otherwise find the log of the restart -- the run that
     * worked -- rather than the one that broke. */
    if (g_file_test (path, G_FILE_TEST_EXISTS)) {
      g_autofree gchar *prev = g_build_filename (dir, "spotify-native.prev.log", NULL);
      g_remove (prev);
      g_rename (path, prev);
    }

    log_stream = g_fopen (path, "w");
  }

  if (!log_stream) {
    g_warning ("log_file: no writable location for a log; console only");
    return;
  }
  log_path = g_steal_pointer (&path);

  g_log_set_writer_func (log_writer, NULL, NULL);

  /* First line of every log, so a pasted file identifies itself. */
  g_message ("log_file: writing this session to %s", log_path);
}

void
spotifygtk_log_file_shutdown (void)
{
  g_mutex_lock (&log_mutex);
  if (log_stream) {
    fflush (log_stream);
    fclose (log_stream);
    log_stream = NULL;
  }
  g_mutex_unlock (&log_mutex);
  g_clear_pointer (&log_path, g_free);
}

const gchar *
spotifygtk_log_file_path (void)
{
  return log_path;
}

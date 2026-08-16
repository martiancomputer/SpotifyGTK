/*
 * apresolve.c — Access point discovery.
 */

#include "config.h"
#include "apresolve.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* Cached for the process lifetime; see the header for why. */
static gchar **cached_hosts = NULL;
static gboolean cache_attempted = FALSE;

gchar **
spotifygtk_apresolve_parse (const gchar *body, gssize len, GError **error)
{
  return spotifygtk_apresolve_parse_type (body, len, "accesspoint", error);
}

gchar **
spotifygtk_apresolve_parse_type (const gchar *body, gssize len,
                                 const gchar *member, GError **error)
{
  g_return_val_if_fail (body != NULL, NULL);

  g_autoptr(JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body, len, error))
    return NULL;

  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "apresolve response was not a JSON object");
    return NULL;
  }

  JsonObject *obj = json_node_get_object (root);
  if (!json_object_has_member (obj, member)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "apresolve response carried no '%s' member", member);
    return NULL;
  }

  JsonNode *ap_node = json_object_get_member (obj, member);
  if (!JSON_NODE_HOLDS_ARRAY (ap_node)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "apresolve '%s' was not an array", member);
    return NULL;
  }

  JsonArray *array = json_node_get_array (ap_node);
  GPtrArray *hosts = g_ptr_array_new ();

  for (guint i = 0; i < json_array_get_length (array); i++) {
    const gchar *entry = json_array_get_string_element (array, i);

    /* Entries are "host:port". Anything without a colon cannot be split by
     * the caller, so drop it rather than passing a hostname with no port
     * down to the connect path. */
    if (!entry || !*entry || !strchr (entry, ':'))
      continue;

    g_ptr_array_add (hosts, g_strdup (entry));
  }

  if (hosts->len == 0) {
    g_ptr_array_free (hosts, TRUE);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "apresolve returned no usable '%s' hosts", member);
    return NULL;
  }

  g_ptr_array_add (hosts, NULL);
  return (gchar **) g_ptr_array_free (hosts, FALSE);
}

static void
on_apresolve_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask)  task = user_data;
  g_autoptr(GError) err  = NULL;

  g_autoptr(GBytes) bytes =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);

  cache_attempted = TRUE;

  if (!bytes) {
    g_task_return_error (task, g_steal_pointer (&err));
    return;
  }

  gsize len = 0;
  const gchar *body = g_bytes_get_data (bytes, &len);

  gchar **hosts = spotifygtk_apresolve_parse (body, (gssize) len, &err);
  if (!hosts) {
    g_task_return_error (task, g_steal_pointer (&err));
    return;
  }

  g_strfreev (cached_hosts);
  cached_hosts = g_strdupv (hosts);

  g_message ("apresolve: %u access point(s) available", g_strv_length (hosts));
  g_task_return_pointer (task, hosts, (GDestroyNotify) g_strfreev);
}

void
spotifygtk_apresolve_get_async (GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);

  if (cached_hosts) {
    g_task_return_pointer (task, g_strdupv (cached_hosts),
                           (GDestroyNotify) g_strfreev);
    g_object_unref (task);
    return;
  }

  /* One shot per process. If the first attempt failed, fall back to SRV
   * rather than retrying on every connection -- the connect path already
   * retries, and stacking a second retry loop underneath it would multiply
   * the delay before a real failure surfaces. */
  if (cache_attempted) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "apresolve already failed once this run");
    g_object_unref (task);
    return;
  }

  SoupSession *session =
    soup_session_new_with_options ("user-agent", "spotify-native/" APP_VERSION, NULL);
  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, APRESOLVE_URL);

  soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT, cancellable,
                                    on_apresolve_response, task);

  /* The session must outlive the request; hand ownership to the message,
   * which the soup call keeps alive until the callback runs. */
  g_object_set_data_full (G_OBJECT (msg), "owning-session", session, g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "owning-message", msg, g_object_unref);
}

gchar **
spotifygtk_apresolve_get_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

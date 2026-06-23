#include "config.h"
#include "connect.h"
#include <string.h>

struct _SpotifyConnectDevice {
  GObject         parent_instance;
  SpotifyMercury *mercury;
  gchar          *device_name;
  gchar          *device_id;
  guint64         sub_id;
  gboolean        registered;
};

enum { SIG_REMOTE_COMMAND, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (SpotifyConnectDevice, spotifygtk_connect_device, G_TYPE_OBJECT)

static gchar *
generate_device_id (void)
{
  /* Spotify Connect device IDs are typically a 40-char hex string
   * (looks like a SHA1 hash of something device-specific). A random
   * value is fine functionally — it just needs to be stable across
   * the process lifetime, which a once-generated GUID-style id gives
   * us; persisting it across restarts (so "this device" stays
   * recognized) is a nice-to-have for later, stored alongside the
   * OAuth tokens. */
  guint8 raw[20];
  GRand *r = g_rand_new ();
  for (int i = 0; i < 20; i++) raw[i] = (guint8) g_rand_int_range (r, 0, 256);
  g_rand_free (r);

  GString *hex = g_string_new (NULL);
  for (int i = 0; i < 20; i++) g_string_append_printf (hex, "%02x", raw[i]);
  return g_string_free (hex, FALSE);
}

static void
on_mercury_event (MercuryResponse *response, gpointer user_data)
{
  SpotifyConnectDevice *self = SPOTIFYGTK_CONNECT_DEVICE (user_data);

  if (!response || !response->parts || response->parts->len == 0) return;

  GBytes *part = g_ptr_array_index (response->parts, 0);
  gsize len = 0;
  const guint8 *data = g_bytes_get_data (part, &len);
  g_autofree gchar *json = g_strndup ((const gchar *) data, len);

  g_signal_emit (self, signals[SIG_REMOTE_COMMAND], 0, json);
}

void
spotifygtk_connect_device_register (SpotifyConnectDevice *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONNECT_DEVICE (self));
  if (self->registered) return;

  g_autofree gchar *uri = g_strdup_printf ("hm://connect-state/v1/devices/%s", self->device_id);
  self->sub_id = spotifygtk_mercury_subscribe (self->mercury, uri, on_mercury_event, self);

  /* Device-state announcement payload would be a protobuf-encoded
   * PutStateRequest in the real protocol (device name, capabilities,
   * volume, is_active flag). Sending a minimal JSON-ish placeholder
   * here keeps the subscription path testable independent of the
   * exact protobuf schema, which would need to be pulled from
   * Spotify's published .proto definitions before this fully works. */
  g_autofree gchar *announce = g_strdup_printf (
    "{\"device_name\":\"%s\",\"device_id\":\"%s\",\"is_active\":false}",
    self->device_name, self->device_id);
  g_autoptr(GBytes) payload = g_bytes_new (announce, strlen (announce));

  spotifygtk_mercury_request (self->mercury, MERCURY_METHOD_SEND, uri, payload, NULL, NULL);

  self->registered = TRUE;
  g_message ("Spotify Connect: registered device '%s' (%s)", self->device_name, self->device_id);
}

void
spotifygtk_connect_device_unregister (SpotifyConnectDevice *self)
{
  g_return_if_fail (SPOTIFYGTK_IS_CONNECT_DEVICE (self));
  if (!self->registered) return;

  spotifygtk_mercury_unsubscribe (self->mercury, self->sub_id);
  self->registered = FALSE;
}

static void
spotifygtk_connect_device_dispose (GObject *object)
{
  SpotifyConnectDevice *self = SPOTIFYGTK_CONNECT_DEVICE (object);
  spotifygtk_connect_device_unregister (self);
  g_clear_object (&self->mercury);
  G_OBJECT_CLASS (spotifygtk_connect_device_parent_class)->dispose (object);
}

static void
spotifygtk_connect_device_finalize (GObject *object)
{
  SpotifyConnectDevice *self = SPOTIFYGTK_CONNECT_DEVICE (object);
  g_free (self->device_name);
  g_free (self->device_id);
  G_OBJECT_CLASS (spotifygtk_connect_device_parent_class)->finalize (object);
}

static void
spotifygtk_connect_device_class_init (SpotifyConnectDeviceClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  oc->dispose  = spotifygtk_connect_device_dispose;
  oc->finalize = spotifygtk_connect_device_finalize;

  signals[SIG_REMOTE_COMMAND] =
    g_signal_new ("remote-command", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
spotifygtk_connect_device_init (SpotifyConnectDevice *self)
{
  self->device_id = generate_device_id ();
}

SpotifyConnectDevice *
spotifygtk_connect_device_new (SpotifyMercury *mercury, const gchar *device_name)
{
  SpotifyConnectDevice *self = g_object_new (SPOTIFYGTK_TYPE_CONNECT_DEVICE, NULL);
  self->mercury     = g_object_ref (mercury);
  self->device_name = g_strdup (device_name);
  return self;
}

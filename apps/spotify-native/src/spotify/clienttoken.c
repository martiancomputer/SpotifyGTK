/*
 * clienttoken.c — client-token exchange implementation.
 * See clienttoken.h for scope/rationale.
 *
 * client_version is librespot's own SPOTIFY_SEMANTIC_VERSION
 * ("1.2.52.442", core/src/version.rs) -- reused verbatim rather than
 * invented, since this identifies the client to a server that may
 * validate it against known-good version strings.
 *
 * connectivity_sdk_data is now populated with device_id and Linux
 * platform details (system_name/release/version/hardware via uname(2)),
 * matching what librespot's spclient.rs sends for Linux. Its absence
 * caused Spotify to return HTTP 400 with a zero-length body.
 *
 * Field layout (all proto3, small field numbers):
 *   ClientTokenRequest (field 1 = request_type varint, field 2 = ClientDataRequest)
 *   ClientDataRequest  (field 1 = client_version, field 2 = client_id,
 *                       field 3 = ConnectivitySdkData)
 *   ConnectivitySdkData (field 1 = PlatformSpecificData, field 2 = device_id)
 *   PlatformSpecificData.desktop_linux (oneof field 5 = NativeDesktopLinuxData)
 *   NativeDesktopLinuxData (field 1 = system_name, field 2 = system_release,
 *                           field 3 = system_version, field 4 = hardware)
 */


#include "config.h"
#include "clienttoken.h"
#include "protobuf_min.h"

#include <libsoup/soup.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#define SPOTIFY_SEMANTIC_VERSION "1.2.52.442"

struct _SpotifyClientToken {
  GObject      parent_instance;
  SoupSession *session;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (SpotifyClientToken, spotifygtk_client_token, G_TYPE_OBJECT)

typedef struct {
  ClientTokenCallback callback;
  gpointer            user_data;
  SoupMessage        *msg;  /* kept alive to read status after the read completes */
} RequestClosure;

static gchar *
hex_preview (const guint8 *data, gsize len, gsize max_bytes)
{
  gsize show = MIN (len, max_bytes);
  GString *s = g_string_sized_new (show * 2 + 8);
  for (gsize i = 0; i < show; i++)
    g_string_append_printf (s, "%02x", data[i]);
  if (len > max_bytes)
    g_string_append_printf (s, "...(%" G_GSIZE_FORMAT " more bytes)", len - max_bytes);
  return g_string_free (s, FALSE);
}

static const gchar *
challenge_type_name (guint64 type)
{
  switch (type) {
    case 0: return "CHALLENGE_UNKNOWN";
    case 1: return "CHALLENGE_CLIENT_SECRET_HMAC";
    case 2: return "CHALLENGE_EVALUATE_JS";
    case 3: return "CHALLENGE_HASH_CASH";
    default: return "(unrecognized challenge type value)";
  }
}

static void
on_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RequestClosure    *cl  = user_data;
  g_autoptr(GError)  err = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &err);
  guint status = soup_message_get_status (cl->msg);

  if (!bytes) {
    g_warning ("clienttoken: request failed (HTTP status %u): %s -- proceeding without a client-token",
              status, err ? err->message : "unknown");
    if (cl->callback) cl->callback (NULL, cl->user_data);
    g_object_unref (cl->msg);
    g_free (cl);
    return;
  }

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);

  g_message ("clienttoken: response HTTP %u, %" G_GSIZE_FORMAT " bytes", status, len);

  /* ClientTokenResponse: response_type (field 1, varint), oneof
   * granted_token (field 2, embedded GrantedTokenResponse) or
   * challenges (field 3, embedded ChallengesResponse). All per
   * clienttoken_http.proto, proto3 field numbers. */
  const guint8 *granted_data = NULL; gsize granted_len = 0;
  if (pb_find_bytes_field (data, len, 2, &granted_data, &granted_len)) {
    const guint8 *token_data = NULL; gsize token_len = 0;
    if (pb_find_bytes_field (granted_data, granted_len, 1, &token_data, &token_len)) {
      g_autofree gchar *token = g_strndup ((const gchar *) token_data, token_len);
      g_message ("clienttoken: obtained a client-token");
      if (cl->callback) cl->callback (token, cl->user_data);
      g_bytes_unref (bytes);
      g_object_unref (cl->msg);
      g_free (cl);
      return;
    }
  }

  /* Distinguish a genuine ChallengesResponse (field 3) -- an actual
   * anti-abuse proof-of-work/JS-eval/HMAC challenge, which is
   * expected and even likely for a from-scratch client hitting this
   * specific endpoint for the first time -- from something
   * genuinely unparseable (wrong field number on our end, an error
   * page, a non-protobuf body), which would instead point at a real
   * bug in this request rather than Spotify's anti-abuse system
   * doing its job. ChallengesResponse.challenges is field 2, repeated
   * Challenge; Challenge.type is field 1. */
  const guint8 *challenges_data = NULL; gsize challenges_len = 0;
  if (pb_find_bytes_field (data, len, 3, &challenges_data, &challenges_len)) {
    g_autofree gchar *types = g_strdup ("");
    gsize pos = 0;
    guint32 field_num; PbWireType wire_type;
    const guint8 *fdata; gsize flen; guint64 fvarint;
    guint challenge_count = 0;

    while (pb_read_field (challenges_data, challenges_len, &pos, &field_num, &wire_type,
                          &fdata, &flen, &fvarint)) {
      if (field_num == 2 && wire_type == PB_WIRE_LENGTH_DELIMITED) {
        challenge_count++;
        guint64 challenge_type = 0;
        pb_find_varint_field (fdata, flen, 1, &challenge_type);
        g_autofree gchar *old_types = types;
        types = g_strdup_printf ("%s%s%s", old_types, *old_types ? ", " : "",
                                 challenge_type_name (challenge_type));
      }
    }

    g_message ("clienttoken: server issued a real anti-abuse challenge (%u challenge(s): %s) -- "
              "this is Spotify's proof-of-work/verification system, not a bug in the request. "
              "Solving it (HashCash/JS-eval/HMAC) is separate, unimplemented work. "
              "Proceeding without a client-token.", challenge_count, types);
  } else {
    /* Neither granted_token nor challenges parsed -- genuinely
     * unexpected. Dump enough to actually debug rather than guess. */
    g_autofree gchar *preview = hex_preview (data, len, 64);
    g_warning ("clienttoken: response has neither granted_token (field 2) nor challenges "
              "(field 3) -- HTTP %u, %" G_GSIZE_FORMAT " bytes, hex preview: %s",
              status, len, preview);
  }

  if (cl->callback) cl->callback (NULL, cl->user_data);
  g_bytes_unref (bytes);
  g_object_unref (cl->msg);
  g_free (cl);
}

void
spotifygtk_client_token_request (SpotifyClientToken *self, const gchar *client_id,
                                 const gchar *device_id,
                                 ClientTokenCallback callback, gpointer user_data)
{
  g_return_if_fail (SPOTIFYGTK_IS_CLIENT_TOKEN (self));

  /* The platform block is a oneof, and the variants are not merely different
   * field numbers -- Linux sends four *strings* from uname, Windows sends
   * *integers*. Sending the Linux shape from Windows is not a cosmetic lie the
   * server tolerates; getting this message wrong is what returns HTTP 400 with
   * a zero-length body (see the file header). Both layouts are transcribed from
   * connectivity.proto and librespot's spclient.rs. */
  g_autoptr(GByteArray) platform_data = g_byte_array_new ();

#ifdef G_OS_WIN32
  /* NativeDesktopWindowsData (oneof field 4):
   *   os_version (1), os_build (3), platform_id (4),
   *   unknown_value_6 (6), image_file_machine (7), pe_machine (8),
   *   unknown_value_10 (10)
   *
   * platform_id 2 is System.PlatformID.Win32NT. The machine constants are
   * IMAGE_FILE_MACHINE_* / PE Machine values; 34404 (0x8664) is x86_64.
   * unknown_value_6 and unknown_value_10 are named that way in the schema
   * itself -- librespot sends 9 and true, and so do we, because nothing here
   * knows what they mean and inventing values is how the 400 happens. */
  OSVERSIONINFOEXW osv = { 0 };
  osv.dwOSVersionInfoSize = sizeof (osv);

  /* GetVersionEx() lies about the version for apps without a compatibility
   * manifest (it caps at 6.2 on Windows 8+). RtlGetVersion does not, and is
   * the documented way out of that; fall back only if it cannot be resolved. */
  HMODULE ntdll = GetModuleHandleW (L"ntdll.dll");
  LONG (WINAPI *rtl_get_version) (OSVERSIONINFOEXW *) = NULL;
  if (ntdll)
    rtl_get_version = (LONG (WINAPI *) (OSVERSIONINFOEXW *))
                        (void *) GetProcAddress (ntdll, "RtlGetVersion");

  gint32 os_version = 10, os_build = 0;
  if (rtl_get_version && rtl_get_version (&osv) == 0) {
    os_version = (gint32) osv.dwMajorVersion;
    os_build   = (gint32) osv.dwBuildNumber;
  } else {
    g_warning ("clienttoken: RtlGetVersion unavailable; using static Windows fields");
  }

  gint32 machine;
#if defined(_M_ARM64) || defined(__aarch64__)
  machine = 43620;            /* ARM64 */
#elif defined(_M_ARM) || defined(__arm__)
  machine = 448;              /* ARM */
#elif defined(_M_X64) || defined(__x86_64__)
  machine = 34404;            /* x86_64 */
#else
  machine = 332;              /* x86 */
#endif

  g_autoptr(GByteArray) win_data = g_byte_array_new ();
  pb_write_varint_field (win_data, 1,  (guint64) os_version);
  pb_write_varint_field (win_data, 3,  (guint64) os_build);
  pb_write_varint_field (win_data, 4,  2);
  pb_write_varint_field (win_data, 6,  9);
  pb_write_varint_field (win_data, 7,  (guint64) machine);
  pb_write_varint_field (win_data, 8,  (guint64) machine);
  pb_write_varint_field (win_data, 10, 1);

  pb_write_message_field (platform_data, 4, win_data->data, win_data->len);
#else
  /* NativeDesktopLinuxData (oneof field 5):
   *   system_name (1) = uname -s, system_release (2) = -r,
   *   system_version (3) = -v, hardware (4) = -m */
  struct utsname uts;
  if (uname (&uts) != 0) {
    /* Non-fatal: fall back to static strings librespot would also use. */
    g_warning ("clienttoken: uname() failed, using static Linux platform fields");
    uts.sysname[0]  = '\0'; g_strlcpy (uts.sysname,  "Linux",   sizeof (uts.sysname));
    uts.release[0]  = '\0'; g_strlcpy (uts.release,  "0",       sizeof (uts.release));
    uts.version[0]  = '\0'; g_strlcpy (uts.version,  "0",       sizeof (uts.version));
    uts.machine[0]  = '\0'; g_strlcpy (uts.machine,  "x86_64",  sizeof (uts.machine));
  }

  g_autoptr(GByteArray) linux_data = g_byte_array_new ();
  pb_write_bytes_field (linux_data, 1, (const guint8 *) uts.sysname,  strlen (uts.sysname));
  pb_write_bytes_field (linux_data, 2, (const guint8 *) uts.release,  strlen (uts.release));
  pb_write_bytes_field (linux_data, 3, (const guint8 *) uts.version,  strlen (uts.version));
  pb_write_bytes_field (linux_data, 4, (const guint8 *) uts.machine,  strlen (uts.machine));

  pb_write_message_field (platform_data, 5, linux_data->data, linux_data->len);
#endif

  /* ConnectivitySdkData:
   *   platform_specific_data (field 1, embedded PlatformSpecificData)
   *   device_id              (field 2, string) */
  g_autoptr(GByteArray) sdk_data = g_byte_array_new ();
  pb_write_message_field (sdk_data, 1, platform_data->data, platform_data->len);
  pb_write_bytes_field   (sdk_data, 2, (const guint8 *) device_id, strlen (device_id));

  /* ClientDataRequest:
   *   client_version       (field 1, string)
   *   client_id            (field 2, string)
   *   connectivity_sdk_data (field 3, embedded ConnectivitySdkData) */
  g_autoptr(GByteArray) client_data = g_byte_array_new ();
  pb_write_bytes_field   (client_data, 1, (const guint8 *) SPOTIFY_SEMANTIC_VERSION,
                          strlen (SPOTIFY_SEMANTIC_VERSION));
  pb_write_bytes_field   (client_data, 2, (const guint8 *) client_id, strlen (client_id));
  pb_write_message_field (client_data, 3, sdk_data->data, sdk_data->len);

  /* ClientTokenRequest:
   *   request_type (field 1, varint, REQUEST_CLIENT_DATA_REQUEST = 1)
   *   client_data  (field 2, embedded ClientDataRequest, oneof "request") */
  g_autoptr(GByteArray) request = g_byte_array_new ();
  pb_write_varint_field  (request, 1, 1);
  pb_write_message_field (request, 2, client_data->data, client_data->len);

  g_message ("clienttoken: sending request (kernel=%s arch=%s device_id=%.8s...)",
             uts.release, uts.machine, device_id ? device_id : "(null)");

  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, CLIENTTOKEN_URL);
  soup_message_headers_replace (soup_message_get_request_headers (msg),
                                "Accept", "application/x-protobuf");

  GBytes *body = g_bytes_new (request->data, request->len);
  soup_message_set_request_body_from_bytes (msg, "application/x-protobuf", body);
  g_bytes_unref (body);

  RequestClosure *cl = g_new0 (RequestClosure, 1);
  cl->callback  = callback;
  cl->user_data = user_data;
  cl->msg       = g_object_ref (msg);

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT, self->cancellable,
                                    on_response, cl);
  g_object_unref (msg);
}

static void
spotifygtk_client_token_dispose (GObject *object)
{
  SpotifyClientToken *self = SPOTIFYGTK_CLIENT_TOKEN (object);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
  G_OBJECT_CLASS (spotifygtk_client_token_parent_class)->dispose (object);
}

static void
spotifygtk_client_token_class_init (SpotifyClientTokenClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_client_token_dispose;
}

static void
spotifygtk_client_token_init (SpotifyClientToken *self)
{
  self->session = soup_session_new_with_options ("user-agent", "spotify-native/" APP_VERSION, NULL);
}

SpotifyClientToken *
spotifygtk_client_token_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_CLIENT_TOKEN, NULL);
}

void spotifygtk_client_token_set_cancellable (SpotifyClientToken *self, GCancellable *cancellable)
{ g_set_object (&self->cancellable, cancellable); }

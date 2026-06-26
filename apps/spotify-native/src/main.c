/*
 * main.c — spotify-native development harness.
 *
 * Not a real client yet. This exists so the engine pieces (Shannon
 * cipher, audio decode, audio output) have somewhere to be exercised
 * and sanity-checked as they're built out, before there's an actual
 * AP login flow or UI to drive them.
 *
 * What this currently does: a Shannon cipher round-trip self-test --
 * key+nonce a sender and an independent receiver identically, encrypt
 * a message on one side, decrypt on the other, verify it matches and
 * the two MACs agree. That's real signal that the ported algorithm
 * is internally consistent (encrypt/decrypt are inverses, MAC
 * accumulation matches between directions) -- it does NOT prove
 * interop with Spotify's actual servers, since that needs a live AP
 * connection and login, which isn't built yet.
 */

#include "config.h"
#include "spotify/shannon.h"

#include <glib.h>
#include <string.h>

static gboolean
run_shannon_selftest (void)
{
  const guint8 key[16]   = "0123456789abcdef";
  const guint8 nonce[8]  = "spotgtk!";
  const gchar *message   = "spotify-native engine harness -- shannon round-trip check";
  gsize        msg_len   = strlen (message);

  ShannonCipher sender, receiver;
  shannon_key_setup (&sender,   key, sizeof (key));
  shannon_key_setup (&receiver, key, sizeof (key));
  shannon_nonce (&sender,   nonce, sizeof (nonce));
  shannon_nonce (&receiver, nonce, sizeof (nonce));

  g_autofree guint8 *buf = g_malloc (msg_len);
  memcpy (buf, message, msg_len);

  shannon_encrypt (&sender, buf, msg_len);

  gboolean garbled_in_transit = (memcmp (buf, message, msg_len) != 0);

  shannon_decrypt (&receiver, buf, msg_len);

  gboolean roundtrip_ok = (memcmp (buf, message, msg_len) == 0);

  guint8 sender_mac[4], receiver_mac[4];
  shannon_finish (&sender,   sender_mac,   sizeof (sender_mac));
  shannon_finish (&receiver, receiver_mac, sizeof (receiver_mac));
  gboolean macs_match = (memcmp (sender_mac, receiver_mac, sizeof (sender_mac)) == 0);

  g_message ("[shannon-selftest] ciphertext != plaintext in transit: %s",
            garbled_in_transit ? "yes (expected)" : "NO -- cipher did nothing, something's wrong");
  g_message ("[shannon-selftest] decrypted message matches original: %s",
            roundtrip_ok ? "yes" : "NO -- round-trip failed");
  g_message ("[shannon-selftest] sender/receiver MACs agree: %s",
            macs_match ? "yes" : "NO -- MAC accumulation mismatch");

  return garbled_in_transit && roundtrip_ok && macs_match;
}

int
main (int argc, char *argv[])
{
  (void) argc; (void) argv;

  g_message ("=== spotify-native engine harness (%s build) ===", APP_PROFILE);
  g_message ("    PipeWire: %s", HAVE_PIPEWIRE ? "yes" : "no");
  g_message ("    PulseAudio: %s", HAVE_PULSE ? "yes" : "no");
  g_message ("    ALSA: %s", HAVE_ALSA ? "yes" : "no");
  g_message ("    OpenSSL (CDN decrypt): %s", HAVE_OPENSSL ? "yes" : "no");
  g_message ("");
  g_message ("Not a real client yet -- no AP login, no playback. See README.");
  g_message ("");

  gboolean ok = run_shannon_selftest ();

  if (!ok) {
    g_warning ("Shannon self-test FAILED -- see messages above");
    return 1;
  }

  g_message ("Shannon self-test passed.");
  return 0;
}

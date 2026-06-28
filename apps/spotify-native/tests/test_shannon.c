/*
 * test_shannon.c — tests for the real, ported Shannon cipher.
 *
 * Previously these tested the honest-stub round function and
 * documented that encrypt() was a no-op (ciphertext == plaintext) by
 * design at the time. Since the real cipher landed (ported from the
 * `shannon` crate, see THIRD_PARTY_LICENSES), that's no longer true --
 * test_roundtrip below replaces the old test_roundtrip_stub with the
 * actual expected behavior: encryption changes the bytes, and
 * decrypt() on an independently-keyed receiver recovers them.
 */

#include <glib.h>
#include <string.h>
#include "shannon.h"

static void
test_key_setup_no_crash (void)
{
  ShannonCipher cipher;
  guint8 key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
  shannon_key_setup (&cipher, key, sizeof (key));
  /* Reaching here without crashing is the assertion. */
  g_assert_true (TRUE);
}

static void
test_roundtrip (void)
{
  ShannonCipher enc, dec;
  guint8 key[16] = {0};
  guint8 nonce[8] = {0};

  shannon_key_setup (&enc, key, sizeof (key));
  shannon_nonce (&enc, nonce, sizeof (nonce));

  shannon_key_setup (&dec, key, sizeof (key));
  shannon_nonce (&dec, nonce, sizeof (nonce));

  guint8 plaintext[32];
  for (int i = 0; i < 32; i++) plaintext[i] = (guint8) i;

  guint8 buf[32];
  memcpy (buf, plaintext, sizeof (buf));

  shannon_encrypt (&enc, buf, sizeof (buf));

  /* Real encryption should change the bytes -- this is the assertion
   * that correctly started failing once the stub was replaced, which
   * is the intended signal, not a regression. */
  g_assert_false (memcmp (buf, plaintext, sizeof (plaintext)) == 0);

  shannon_decrypt (&dec, buf, sizeof (buf));

  /* An independently-keyed-and-nonced receiver should recover the
   * original bytes exactly. */
  g_assert_cmpmem (buf, sizeof (buf), plaintext, sizeof (plaintext));
}

static void
test_mac_agreement (void)
{
  /* Sender and receiver should derive identical MACs after a
   * matching encrypt/decrypt pair, given they accumulate the MAC
   * over the plaintext value either way (see shannon.c). */
  ShannonCipher enc, dec;
  guint8 key[16]  = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11, 0x22,
                     0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00};
  guint8 nonce[4] = {0x01, 0x02, 0x03, 0x04};

  shannon_key_setup (&enc, key, sizeof (key));
  shannon_nonce (&enc, nonce, sizeof (nonce));
  shannon_key_setup (&dec, key, sizeof (key));
  shannon_nonce (&dec, nonce, sizeof (nonce));

  guint8 buf[17] = "shannon test msg";  /* deliberately not a multiple of 4 */
  shannon_encrypt (&enc, buf, sizeof (buf));
  shannon_decrypt (&dec, buf, sizeof (buf));

  guint8 mac_enc[4], mac_dec[4];
  shannon_finish (&enc, mac_enc, sizeof (mac_enc));
  shannon_finish (&dec, mac_dec, sizeof (mac_dec));

  g_assert_cmpmem (mac_enc, sizeof (mac_enc), mac_dec, sizeof (mac_dec));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/shannon/key-setup-no-crash", test_key_setup_no_crash);
  g_test_add_func ("/shannon/roundtrip",          test_roundtrip);
  g_test_add_func ("/shannon/mac-agreement",      test_mac_agreement);
  return g_test_run ();
}

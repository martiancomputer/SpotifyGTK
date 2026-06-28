#include <glib.h>
#include <string.h>
#include "spotify/dh.h"

static void
test_dh_agreement (void)
{
  /* Two independent local keypairs (standing in for "us" and "the
   * server") should arrive at the identical shared secret -- this is
   * the actual property that matters, testable with zero network
   * access since it's just the math, not the protocol around it. */
  DhLocalKeys alice, bob;
  dh_local_keys_generate (&alice);
  dh_local_keys_generate (&bob);

  /* Public keys should differ (overwhelmingly likely with real
   * randomness) and shouldn't be all-zero (would indicate generation
   * silently failed, e.g. built without OpenSSL). */
  g_assert_cmpint (memcmp (alice.public_key, bob.public_key, DH_KEY_BYTES), !=, 0);

  guint8 zero[DH_KEY_BYTES] = {0};
  g_assert_cmpint (memcmp (alice.public_key, zero, DH_KEY_BYTES), !=, 0);
  g_assert_cmpint (memcmp (bob.public_key, zero, DH_KEY_BYTES), !=, 0);

  guint8 secret_a[DH_KEY_BYTES], secret_b[DH_KEY_BYTES];

  gboolean ok_a = dh_compute_shared_secret (&alice, bob.public_key, DH_KEY_BYTES,
                                            secret_a, sizeof (secret_a));
  gboolean ok_b = dh_compute_shared_secret (&bob, alice.public_key, DH_KEY_BYTES,
                                            secret_b, sizeof (secret_b));

  g_assert_true (ok_a);
  g_assert_true (ok_b);
  g_assert_cmpmem (secret_a, sizeof (secret_a), secret_b, sizeof (secret_b));
}

static void
test_dh_keys_differ_each_call (void)
{
  /* Sanity check against an accidentally-deterministic RNG path. */
  DhLocalKeys k1, k2;
  dh_local_keys_generate (&k1);
  dh_local_keys_generate (&k2);
  g_assert_cmpint (memcmp (k1.private_key, k2.private_key, DH_PRIVATE_KEY_BYTES), !=, 0);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/dh/shared-secret-agreement", test_dh_agreement);
  g_test_add_func ("/dh/keys-differ-each-call",   test_dh_keys_differ_each_call);
  return g_test_run ();
}

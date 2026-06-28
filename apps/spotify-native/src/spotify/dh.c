/*
 * dh.c — Diffie-Hellman implementation via OpenSSL's BIGNUM API.
 * See dh.h for the byte-order note on private keys.
 */

#include "config.h"
#include "dh.h"
#include "handshake_constants.h"

#if HAVE_OPENSSL
#include <openssl/bn.h>
#include <openssl/rand.h>
#endif
#include <string.h>

void
dh_local_keys_generate (DhLocalKeys *keys)
{
  memset (keys, 0, sizeof (*keys));

#if HAVE_OPENSSL
  RAND_bytes (keys->private_key, DH_PRIVATE_KEY_BYTES);

  BIGNUM *p   = BN_bin2bn (DH_PRIME_BYTES, sizeof (DH_PRIME_BYTES), NULL);
  BIGNUM *g   = BN_new ();
  BIGNUM *x   = BN_bin2bn (keys->private_key, DH_PRIVATE_KEY_BYTES, NULL);
  BIGNUM *pub = BN_new ();
  BN_CTX *ctx = BN_CTX_new ();
  BN_set_word (g, DH_GENERATOR);

  if (BN_mod_exp (pub, g, x, p, ctx) == 1) {
    int pub_len = BN_num_bytes (pub);
    if (pub_len <= DH_KEY_BYTES)
      BN_bn2bin (pub, keys->public_key + (DH_KEY_BYTES - pub_len));
    else
      g_warning ("dh: computed public key unexpectedly larger than the prime");
  } else {
    g_warning ("dh: BN_mod_exp failed during local key generation");
  }

  BN_free (p);
  BN_free (g);
  BN_free (x);
  BN_free (pub);
  BN_CTX_free (ctx);
#else
  g_warning ("dh: built without OpenSSL -- cannot generate DH keys");
#endif
}

gboolean
dh_compute_shared_secret (const DhLocalKeys *keys,
                          const guint8 *remote_pubkey, gsize remote_pubkey_len,
                          guint8 *out_secret, gsize out_secret_len)
{
  if (out_secret_len < DH_KEY_BYTES) return FALSE;

#if HAVE_OPENSSL
  BIGNUM *p          = BN_bin2bn (DH_PRIME_BYTES, sizeof (DH_PRIME_BYTES), NULL);
  BIGNUM *x          = BN_bin2bn (keys->private_key, DH_PRIVATE_KEY_BYTES, NULL);
  BIGNUM *remote_pub = BN_bin2bn (remote_pubkey, (int) remote_pubkey_len, NULL);
  BIGNUM *secret     = BN_new ();
  BN_CTX *ctx        = BN_CTX_new ();

  gboolean ok = (BN_mod_exp (secret, remote_pub, x, p, ctx) == 1);

  if (ok) {
    int secret_len = BN_num_bytes (secret);
    if (secret_len > DH_KEY_BYTES) {
      ok = FALSE;
    } else {
      memset (out_secret, 0, DH_KEY_BYTES);
      BN_bn2bin (secret, out_secret + (DH_KEY_BYTES - secret_len));
    }
  }

  BN_free (p);
  BN_free (x);
  BN_free (remote_pub);
  BN_free (secret);
  BN_CTX_free (ctx);
  return ok;
#else
  (void) keys; (void) remote_pubkey; (void) remote_pubkey_len; (void) out_secret;
  g_warning ("dh: built without OpenSSL -- cannot compute shared secret");
  return FALSE;
#endif
}

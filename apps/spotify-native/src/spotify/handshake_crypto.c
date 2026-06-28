/*
 * handshake_crypto.c — HMAC key derivation + RSA signature verify.
 * See handshake_crypto.h for context.
 */

#include "config.h"
#include "handshake_crypto.h"
#include "handshake_constants.h"

#if HAVE_OPENSSL
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#endif
#include <string.h>

gboolean
hs_compute_keys (const guint8 *shared_secret, gsize shared_secret_len,
                 const guint8 *packets, gsize packets_len,
                 guint8 *out_challenge, guint8 *out_send_key, guint8 *out_recv_key)
{
#if HAVE_OPENSSL
  guint8 data[100];  /* 5 rounds x 20-byte SHA-1 HMAC output */
  unsigned int hmac_len = 0;

  for (int i = 1; i <= 5; i++) {
    guint8 counter = (guint8) i;
    HMAC_CTX *ctx = HMAC_CTX_new ();
    if (!ctx) return FALSE;

    HMAC_Init_ex (ctx, shared_secret, (int) shared_secret_len, EVP_sha1 (), NULL);
    HMAC_Update (ctx, packets, packets_len);
    HMAC_Update (ctx, &counter, 1);
    HMAC_Final (ctx, data + (i - 1) * 20, &hmac_len);
    HMAC_CTX_free (ctx);

    if (hmac_len != 20) return FALSE;
  }

  /* challenge = HMAC-SHA1(key = data[0:20], message = packets) */
  HMAC_CTX *ctx2 = HMAC_CTX_new ();
  if (!ctx2) return FALSE;
  HMAC_Init_ex (ctx2, data, 20, EVP_sha1 (), NULL);
  HMAC_Update (ctx2, packets, packets_len);
  HMAC_Final (ctx2, out_challenge, &hmac_len);
  HMAC_CTX_free (ctx2);
  if (hmac_len != HS_CHALLENGE_LEN) return FALSE;

  memcpy (out_send_key, data + 20, HS_SEND_KEY_LEN);
  memcpy (out_recv_key, data + 52, HS_RECV_KEY_LEN);
  return TRUE;
#else
  (void) shared_secret; (void) shared_secret_len; (void) packets; (void) packets_len;
  (void) out_challenge; (void) out_send_key; (void) out_recv_key;
  g_warning ("handshake_crypto: built without OpenSSL -- cannot derive keys");
  return FALSE;
#endif
}

gboolean
hs_verify_server_signature (const guint8 *server_dh_pubkey, gsize server_dh_pubkey_len,
                           const guint8 *signature, gsize signature_len)
{
#if HAVE_OPENSSL
  guint8 hash[SHA_DIGEST_LENGTH];
  SHA1 (server_dh_pubkey, server_dh_pubkey_len, hash);

  BIGNUM *n = BN_bin2bn (AP_SERVER_KEY, sizeof (AP_SERVER_KEY), NULL);
  BIGNUM *e = BN_new ();
  BN_set_word (e, RSA_EXPONENT);

  RSA *rsa = RSA_new ();
  if (!rsa || !n || !e || RSA_set0_key (rsa, n, e, NULL) != 1) {
    g_warning ("handshake_crypto: failed to construct RSA key from AP_SERVER_KEY");
    if (rsa) RSA_free (rsa); else { BN_free (n); BN_free (e); }
    return FALSE;
  }
  /* RSA_set0_key took ownership of n and e on success; don't free separately. */

  EVP_PKEY *pkey = EVP_PKEY_new ();
  if (!pkey || EVP_PKEY_assign_RSA (pkey, rsa) != 1) {
    g_warning ("handshake_crypto: failed to wrap RSA key in EVP_PKEY");
    RSA_free (rsa);
    if (pkey) EVP_PKEY_free (pkey);
    return FALSE;
  }
  /* pkey now owns rsa. */

  EVP_PKEY_CTX *vctx = EVP_PKEY_CTX_new (pkey, NULL);
  gboolean ok = FALSE;
  if (vctx &&
      EVP_PKEY_verify_init (vctx) == 1 &&
      EVP_PKEY_CTX_set_signature_md (vctx, EVP_sha1 ()) == 1 &&
      EVP_PKEY_verify (vctx, signature, signature_len, hash, sizeof (hash)) == 1) {
    ok = TRUE;
  }

  if (vctx) EVP_PKEY_CTX_free (vctx);
  EVP_PKEY_free (pkey);
  return ok;
#else
  (void) server_dh_pubkey; (void) server_dh_pubkey_len; (void) signature; (void) signature_len;
  g_warning ("handshake_crypto: built without OpenSSL -- cannot verify signature");
  return FALSE;
#endif
}

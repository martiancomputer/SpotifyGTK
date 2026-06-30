/*
 * handshake_crypto.c — HMAC key derivation + RSA signature verify.
 * See handshake_crypto.h for context.
 *
 * Uses OpenSSL 3.0's EVP_MAC (HMAC) and OSSL_PARAM_BLD/EVP_PKEY_fromdata
 * (RSA public key construction) rather than the legacy HMAC_CTX_* and
 * RSA_*/EVP_PKEY_assign_RSA APIs, which OpenSSL 3.0 deprecates. Both
 * replacement patterns verified against OpenSSL's own documentation
 * (docs.openssl.org/EVP_MAC, EVP_PKEY-RSA) before writing this, the
 * same standard applied to the protocol constants elsewhere in this
 * codebase: don't guess at API shapes from memory when getting one
 * wrong produces code that compiles but is subtly broken.
 */

#include "config.h"
#include "handshake_crypto.h"
#include "handshake_constants.h"

#if HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#endif
#include <string.h>

#if HAVE_OPENSSL
/* Single round of HMAC-SHA1 via the EVP_MAC interface. Returns TRUE
 * and writes exactly 20 bytes to out on success. */
static gboolean
hmac_sha1 (const guint8 *key, gsize key_len,
          const guint8 *data1, gsize data1_len,
          const guint8 *data2, gsize data2_len,  /* optional second chunk, may be NULL/0 */
          guint8 *out /* 20 bytes */)
{
  gboolean ok = FALSE;
  EVP_MAC *mac = EVP_MAC_fetch (NULL, "HMAC", NULL);
  if (!mac) return FALSE;

  EVP_MAC_CTX *ctx = EVP_MAC_CTX_new (mac);
  if (!ctx) { EVP_MAC_free (mac); return FALSE; }

  OSSL_PARAM params[] = {
    OSSL_PARAM_construct_utf8_string (OSSL_MAC_PARAM_DIGEST, "SHA1", 0),
    OSSL_PARAM_construct_end (),
  };

  if (EVP_MAC_init (ctx, key, key_len, params) == 1 &&
      EVP_MAC_update (ctx, data1, data1_len) == 1 &&
      (data2 == NULL || data2_len == 0 || EVP_MAC_update (ctx, data2, data2_len) == 1)) {
    gsize outlen = 0;
    if (EVP_MAC_final (ctx, out, &outlen, SHA_DIGEST_LENGTH) == 1 && outlen == SHA_DIGEST_LENGTH)
      ok = TRUE;
  }

  EVP_MAC_CTX_free (ctx);
  EVP_MAC_free (mac);
  return ok;
}
#endif

gboolean
hs_compute_keys (const guint8 *shared_secret, gsize shared_secret_len,
                 const guint8 *packets, gsize packets_len,
                 guint8 *out_challenge, guint8 *out_send_key, guint8 *out_recv_key)
{
#if HAVE_OPENSSL
  guint8 data[100];  /* 5 rounds x 20-byte SHA-1 HMAC output */

  for (int i = 1; i <= 5; i++) {
    guint8 counter = (guint8) i;
    if (!hmac_sha1 (shared_secret, shared_secret_len,
                    packets, packets_len, &counter, 1,
                    data + (i - 1) * 20))
      return FALSE;
  }

  /* challenge = HMAC-SHA1(key = data[0:20], message = packets) */
  if (!hmac_sha1 (data, 20, packets, packets_len, NULL, 0, out_challenge))
    return FALSE;

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

  gboolean ok = FALSE;

  BIGNUM *n = BN_bin2bn (AP_SERVER_KEY, sizeof (AP_SERVER_KEY), NULL);
  BIGNUM *e = BN_new ();
  if (!n || !e || BN_set_word (e, RSA_EXPONENT) != 1) {
    g_warning ("handshake_crypto: failed to construct BIGNUMs for AP_SERVER_KEY");
    if (n) BN_free (n);
    if (e) BN_free (e);
    return FALSE;
  }

  /* Build an EVP_PKEY from raw modulus+exponent bytes via the
   * OpenSSL 3.0+ OSSL_PARAM_BLD path -- this is the modern
   * replacement for RSA_new()+RSA_set0_key()+EVP_PKEY_assign_RSA(),
   * all of which OpenSSL 3.0 deprecates. */
  EVP_PKEY_CTX *build_ctx = EVP_PKEY_CTX_new_from_name (NULL, "RSA", NULL);
  OSSL_PARAM_BLD *param_bld = OSSL_PARAM_BLD_new ();
  EVP_PKEY *pkey = NULL;

  if (build_ctx && param_bld &&
      OSSL_PARAM_BLD_push_BN (param_bld, OSSL_PKEY_PARAM_RSA_N, n) == 1 &&
      OSSL_PARAM_BLD_push_BN (param_bld, OSSL_PKEY_PARAM_RSA_E, e) == 1) {
    OSSL_PARAM *params = OSSL_PARAM_BLD_to_param (param_bld);
    if (params) {
      if (EVP_PKEY_fromdata_init (build_ctx) == 1 &&
          EVP_PKEY_fromdata (build_ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) == 1) {
        /* pkey now holds an independent copy of n/e's values; ok so far. */
      }
      OSSL_PARAM_free (params);
    }
  } else {
    g_warning ("handshake_crypto: failed to build OSSL_PARAM set for AP_SERVER_KEY");
  }

  if (param_bld) OSSL_PARAM_BLD_free (param_bld);
  if (build_ctx) EVP_PKEY_CTX_free (build_ctx);
  BN_free (n);
  BN_free (e);

  if (!pkey) {
    g_warning ("handshake_crypto: EVP_PKEY_fromdata failed to construct the AP server key");
    return FALSE;
  }

  EVP_PKEY_CTX *vctx = EVP_PKEY_CTX_new (pkey, NULL);
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

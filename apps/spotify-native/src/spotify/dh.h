/*
 * dh.h — Diffie-Hellman key exchange for the AP handshake.
 *
 * Uses the standard RFC 2409 "Second Oakley Group" (768-bit MODP,
 * generator 2) — see handshake_constants.h. This is a well-known
 * public parameter set, not anything Spotify-specific.
 *
 * Note on private key byte order: librespot interprets its randomly
 * generated private-key bytes as little-endian. This implementation
 * doesn't replicate that, and deliberately so -- it's purely an
 * internal convention for interpreting *our own* randomly generated
 * secret scalar, which is never transmitted or compared against
 * anything external. Any consistent interpretation of a uniformly
 * random byte string produces an equally valid private key; only the
 * resulting public key and final shared secret (computed the same
 * way regardless of this choice) matter for interop, and those are
 * unaffected.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define DH_PRIVATE_KEY_BYTES 95
#define DH_KEY_BYTES         96  /* matches the 768-bit prime's byte width */

typedef struct {
  guint8 private_key[DH_PRIVATE_KEY_BYTES];
  guint8 public_key[DH_KEY_BYTES];   /* g^x mod p, big-endian, zero-padded to full width */
} DhLocalKeys;

/* Generates a fresh random private key and computes the corresponding
 * public key. Call once per AP connection attempt. */
void dh_local_keys_generate (DhLocalKeys *keys);

/* Computes shared_secret = remote_pubkey^our_private_key mod p.
 * out_secret must be at least DH_KEY_BYTES; written zero-padded to
 * full width, big-endian, matching the format the HMAC key
 * derivation expects. Returns FALSE on any OpenSSL BIGNUM failure. */
gboolean dh_compute_shared_secret (const DhLocalKeys *keys,
                                   const guint8 *remote_pubkey, gsize remote_pubkey_len,
                                   guint8 *out_secret, gsize out_secret_len);

G_END_DECLS

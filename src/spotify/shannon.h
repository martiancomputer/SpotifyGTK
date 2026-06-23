/*
 * shannon.h — Shannon stream cipher.
 *
 * Spotify's Access Point transport (the binary TCP protocol used for
 * Mercury/auth/audio-key exchange — NOT the CDN, which is plain HTTPS)
 * is encrypted with the Shannon cipher, a word-oriented stream cipher
 * with built-in MAC support, originally published by Greg Rose (1999,
 * "Helix"-family precursor).
 *
 * STATUS: interface + key-setup scaffolding only. The core round
 * function (sbox/diffusion constants) is intentionally NOT filled in
 * here — see the long comment in shannon.c. Getting a stream cipher's
 * internal constants subtly wrong produces code that compiles, runs,
 * and silently fails to interoperate with Spotify's servers. That's a
 * worse outcome than an honest gap. Before this ships, the constants
 * need to come from a verified primary source (the original Shannon
 * reference implementation), not reconstructed from memory.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SHANNON_KEY_LEN_MAX 32   /* bytes */
#define SHANNON_N           16   /* state size, in 32-bit words */

typedef struct {
  guint32 R[SHANNON_N];     /* main register   */
  guint32 CRC[SHANNON_N];   /* CRC accumulator */
  guint32 initR[SHANNON_N]; /* saved initial state, for re-key  */
  guint32 konst;
  guint32 sbuf;
  gint    nbuf;
} ShannonCipher;

/* Initialise cipher state from a key (also resets the nonce/IV state). */
void shannon_key_setup (ShannonCipher *cipher, const guint8 *key, gsize key_len);

/* Per-packet nonce setup (Spotify re-nonces using a monotonic counter). */
void shannon_nonce (ShannonCipher *cipher, const guint8 *nonce, gsize nonce_len);

/* Encrypt/decrypt in place (Shannon is a synchronous stream cipher — XOR-based). */
void shannon_encrypt (ShannonCipher *cipher, guint8 *buf, gsize len);
void shannon_decrypt  (ShannonCipher *cipher, guint8 *buf, gsize len);

/* Produce the 4-byte MAC for the current stream position. */
void shannon_finish (ShannonCipher *cipher, guint8 *mac_out, gsize mac_len);

G_END_DECLS

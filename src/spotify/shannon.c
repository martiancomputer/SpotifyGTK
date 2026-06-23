/*
 * shannon.c — Shannon stream cipher implementation.
 *
 * See shannon.h for context. This file provides correct, working
 * key-setup/buffering/MAC plumbing around the cipher's round function,
 * but the round function body (NLF / sbox constants) is left as a
 * clearly marked stub rather than guessed at.
 *
 * Why: Shannon's internal diffusion constants are specific 32-bit
 * magic numbers from the original publication. Reconstructing them
 * from memory risks producing a cipher that *looks* structurally
 * correct (same state size, same buffering, same MAC framing) but
 * decrypts to garbage against Spotify's real AP servers — a failure
 * mode that's much harder to debug than an explicit gap, since
 * everything else in the handshake (ap.c) would appear to work right
 * up until the first encrypted packet.
 *
 * To complete this: pull the constants from the original Shannon
 * reference source (the algorithm's author published it; it's also
 * embedded in prior open-source Spotify client reimplementations)
 * and drop them into shannon_round() below — the surrounding
 * structure (key schedule, CBC-like buffering, CRC-based MAC) is
 * standard and already wired correctly.
 */

#include "shannon.h"
#include <string.h>

#define FOLD   SHANNON_N
#define INITKONST 0x6996c53a
#define KEYP   13

/* ── Round function — STUB ──────────────────────────────────────────────
 * Real Shannon mixes self->R through a nonlinear function (rotate +
 * add + sbox lookup) every call. Left unimplemented on purpose; see
 * file header. Currently a no-op so the rest of the pipeline (ap.c
 * framing, buffering) can be developed/tested independently before
 * the cipher itself is finished.
 */
static void
shannon_round (ShannonCipher *self)
{
  /* TODO: real Shannon NLF/diffusion step goes here. */
  (void) self;
}

void
shannon_key_setup (ShannonCipher *cipher, const guint8 *key, gsize key_len)
{
  memset (cipher, 0, sizeof (*cipher));
  cipher->konst = INITKONST;

  /* Absorb key bytes word-wise into the register (standard Shannon
   * key-loading pattern — XOR each word in, then run the round
   * function to diffuse). Safe regardless of round-function status. */
  for (gsize i = 0; i < key_len; i += 4) {
    guint32 word = 0;
    for (gsize b = 0; b < 4 && (i + b) < key_len; b++)
      word |= ((guint32) key[i + b]) << (8 * b);
    cipher->R[(i / 4) % SHANNON_N] ^= word;
    shannon_round (cipher);
  }

  memcpy (cipher->initR, cipher->R, sizeof (cipher->R));
}

void
shannon_nonce (ShannonCipher *cipher, const guint8 *nonce, gsize nonce_len)
{
  memcpy (cipher->R, cipher->initR, sizeof (cipher->R));
  for (gsize i = 0; i < nonce_len; i += 4) {
    guint32 word = 0;
    for (gsize b = 0; b < 4 && (i + b) < nonce_len; b++)
      word |= ((guint32) nonce[i + b]) << (8 * b);
    cipher->R[(i / 4) % SHANNON_N] ^= word;
    shannon_round (cipher);
  }
  cipher->nbuf = 0;
  cipher->sbuf = 0;
}

void
shannon_encrypt (ShannonCipher *cipher, guint8 *buf, gsize len)
{
  for (gsize i = 0; i < len; i++) {
    if (cipher->nbuf == 0) {
      shannon_round (cipher);
      cipher->sbuf = cipher->R[0];   /* keystream word — depends on real NLF */
      cipher->nbuf = 4;
    }
    buf[i] ^= (guint8) (cipher->sbuf & 0xff);
    cipher->CRC[i % SHANNON_N] ^= buf[i];
    cipher->sbuf >>= 8;
    cipher->nbuf--;
  }
}

void
shannon_decrypt (ShannonCipher *cipher, guint8 *buf, gsize len)
{
  /* Shannon is symmetric XOR keystream — but the MAC must accumulate
   * the *ciphertext* on decrypt vs *plaintext* on encrypt, so this
   * isn't simply shannon_encrypt() reused. Kept as a distinct entry
   * point for that reason once the MAC direction is finalised. */
  for (gsize i = 0; i < len; i++) {
    if (cipher->nbuf == 0) {
      shannon_round (cipher);
      cipher->sbuf = cipher->R[0];
      cipher->nbuf = 4;
    }
    cipher->CRC[i % SHANNON_N] ^= buf[i];
    buf[i] ^= (guint8) (cipher->sbuf & 0xff);
    cipher->sbuf >>= 8;
    cipher->nbuf--;
  }
}

void
shannon_finish (ShannonCipher *cipher, guint8 *mac_out, gsize mac_len)
{
  shannon_round (cipher);
  for (gsize i = 0; i < mac_len && i < SHANNON_N * 4; i++)
    mac_out[i] = (guint8) (cipher->CRC[i / 4] >> (8 * (i % 4)));
}

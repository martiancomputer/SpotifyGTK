/*
 * shannon.h — Shannon stream cipher.
 *
 * Ported from the `shannon` Rust crate v0.2.0 (MIT License,
 * Copyright (c) 2016 Paul Liétar), via librespot's dependency on it.
 * See THIRD_PARTY_LICENSES for the full license text and
 * research/auth/ for the porting notes.
 *
 * This is the cipher Spotify's AP transport (the binary TCP protocol
 * used for the handshake, Mercury, audio-key exchange — NOT the CDN,
 * which is plain HTTPS) is encrypted with.
 *
 * Unlike the earlier version of this file, the round function below
 * is the real, verified algorithm — not reconstructed from memory.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SHANNON_N 16   /* state size, in 32-bit words */

typedef struct {
  guint32 R[SHANNON_N];      /* main register */
  guint32 CRC[SHANNON_N];    /* CRC accumulator, also doubles as MAC state */
  guint32 initR[SHANNON_N];  /* saved post-key-load state, restored per nonce() call */
  guint32 konst;
  guint32 sbuf;               /* current keystream word */
  guint32 mbuf;               /* partial-word MAC accumulator for non-word-aligned tails */
  gsize   nbuf;               /* bits remaining in the current partial word (counts down by 8) */
} ShannonCipher;

/* Initialise cipher state from a key. Also usable directly as the
 * per-packet nonce setup IF you pass the nonce bytes here after a
 * fresh shannon_key_setup() — but for re-nonceing an already-keyed
 * cipher (the normal per-packet case), use shannon_nonce() instead,
 * which restores the saved post-key state first. */
void shannon_key_setup (ShannonCipher *cipher, const guint8 *key, gsize key_len);

/* Per-packet nonce setup — restores the saved key-derived state, then
 * folds in the nonce bytes the same way a key would be folded in.
 * Spotify re-nonces with a monotonically incrementing per-direction
 * 32-bit counter, one nonce per packet (see shannon_nonce_u32). */
void shannon_nonce (ShannonCipher *cipher, const guint8 *nonce, gsize nonce_len);

/* Convenience wrapper: nonce from a big-endian-encoded u32 counter,
 * matching librespot's ApCodec::nonce_u32 usage exactly. */
void shannon_nonce_u32 (ShannonCipher *cipher, guint32 n);

/* Encrypt/decrypt in place. Both accumulate the MAC over the
 * plaintext value (not the ciphertext) — this is intentional and
 * matches the reference; encrypt() and decrypt() are NOT
 * interchangeable despite both being XOR-based, because the MAC
 * accumulation order differs (decrypt recovers plaintext first, then
 * MACs it; encrypt MACs the plaintext it was given, then encrypts it). */
void shannon_encrypt (ShannonCipher *cipher, guint8 *buf, gsize len);
void shannon_decrypt  (ShannonCipher *cipher, guint8 *buf, gsize len);

/* Finish MAC computation, writing mac_len bytes of MAC into buf.
 * This consumes/perturbs cipher state — call once per packet, after
 * the matching encrypt()/decrypt() call, not before. */
void shannon_finish (ShannonCipher *cipher, guint8 *buf, gsize mac_len);

/* Convenience: finish() and compare against an expected MAC. Returns
 * TRUE if it matches. */
gboolean shannon_check_mac (ShannonCipher *cipher, const guint8 *expected, gsize expected_len);

G_END_DECLS

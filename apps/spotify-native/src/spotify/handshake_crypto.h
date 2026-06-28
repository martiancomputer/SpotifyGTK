/*
 * handshake_crypto.h — HMAC key derivation + RSA signature
 * verification for the AP handshake.
 *
 * Separate from dh.c (the DH math itself) and ap.c (connection/
 * framing) since these are self-contained crypto operations that
 * benefit from being independently testable.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define HS_CHALLENGE_LEN 20   /* SHA-1 output size */
#define HS_SEND_KEY_LEN  32
#define HS_RECV_KEY_LEN  32

/*
 * Derives the handshake "challenge" (sent back to the server to
 * prove we computed the same shared secret) and the two Shannon
 * cipher keys, from the DH shared secret and the full accumulated
 * handshake bytes (ClientHello + APResponseMessage, wire-format
 * bytes, both directions concatenated in send/receive order).
 *
 * out_challenge must be HS_CHALLENGE_LEN bytes.
 * out_send_key / out_recv_key must each be 32 bytes (HS_SEND_KEY_LEN
 * / HS_RECV_KEY_LEN) -- these become the Shannon cipher keys via
 * shannon_key_setup().
 */
gboolean hs_compute_keys (const guint8 *shared_secret, gsize shared_secret_len,
                         const guint8 *packets, gsize packets_len,
                         guint8 *out_challenge,
                         guint8 *out_send_key,
                         guint8 *out_recv_key);

/*
 * Verifies the server's RSA-SHA1-PKCS1v1.5 signature over its DH
 * public key bytes, using Spotify's hardcoded AP service public key
 * (AP_SERVER_KEY in handshake_constants.h). This is the only thing
 * standing between us and a MITM during the handshake.
 */
gboolean hs_verify_server_signature (const guint8 *server_dh_pubkey, gsize server_dh_pubkey_len,
                                    const guint8 *signature, gsize signature_len);

G_END_DECLS

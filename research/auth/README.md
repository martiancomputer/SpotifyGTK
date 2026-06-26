# Auth research: AP handshake, Diffie-Hellman, Shannon cipher

**Upstream reference:** `librespot-org/librespot`
- `core/src/connection/handshake.rs` — DH exchange, RSA server-key verification
- `core/src/diffie_hellman.rs` — DH parameter generation
- `core/src/connection/codec.rs` — packet framing, nonce-per-packet scheme
- `shannon` crate v0.2.0 (Paul Liétar, MIT) — the cipher itself, ported
  directly into `apps/spotify-native/src/spotify/shannon.c`

## Confirmed findings

**DH parameters are standard, not Spotify-proprietary.** librespot uses the
classic RFC 2409 "Second Oakley Group" — 768-bit MODP, generator 2. This is
a well-known public group, not anything reverse-engineered from Spotify
specifically. Private keys are 95 random bytes (760 bits); public key is
`g^x mod p` via standard binary modular exponentiation.

**RSA server key is hardcoded** in librespot as a 256-byte modulus +
exponent 65537, used to verify the server's signature over its DH public
key during the handshake (prevents MITM). This is Spotify's actual public
key for the AP service — copying the raw bytes (not executable code) for
interoperability is a meaningfully different thing than copying source,
but worth flagging here rather than treating as a non-issue.

**Shannon cipher**: fully ported, see `apps/spotify-native/src/spotify/shannon.c`.
Key MAC-accumulation detail: both encrypt and decrypt accumulate the MAC
over the *plaintext* value, never ciphertext — this differs by direction
in *when* (encrypt MACs before XOR-ing, decrypt MACs after recovering
plaintext) but never *what*.

## Open items

- [ ] Hand-roll the `keyexchange.proto` messages (ClientHello,
      APResponseMessage, ClientResponsePlaintext) in C — schema is small
      enough not to need a full protobuf-c dependency
- [ ] HMAC-SHA1 key expansion (5 rounds → 100 bytes → send_key/recv_key
      split) — not yet ported into `ap.c`
- [ ] Login step itself (sending stored credentials over the now-encrypted
      AP connection) — separate from the DH handshake, not yet researched

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

**Key derivation produces 32-byte keys, not 20.** Easy to get wrong:
`compute_keys()` derives 100 bytes total (5×20-byte HMAC-SHA1 rounds), but
`send_key`/`recv_key` are each `data[20:52]`/`data[52:84]` — 32 bytes apiece,
not 20. This matches `SHANNON_KEY_LEN_MAX 32` already in `shannon.h`. The
first 20 bytes (`data[0:20]`) become the HMAC key for the client's
"challenge" response, not part of either cipher key.

## Open items

- [x] Hand-roll the `keyexchange.proto` messages in C — `protobuf_min.c`,
      tested in `tests/test_protobuf.c`
- [x] HMAC-SHA1 key expansion — `handshake_crypto.c` (`hs_compute_keys`)
- [x] Full handshake wired into `ap.c` (`perform_handshake`) — ported from
      `core/src/connection/handshake.rs`, using the verified constants in
      `handshake_constants.h` and the real Shannon cipher
- [ ] **Not yet tested against a live Spotify server.** Everything above
      is "should be correct per the reference," not "confirmed working" —
      this sandbox can't reach `ap.spotify.com`. `dh.c`'s math is verified
      independently (`test_dh.c`: two local keypairs agree on a shared
      secret), but that doesn't prove interop with Spotify's actual servers
- [x] Login step (`ap.c`: `spotifygtk_ap_session_login()`, ported from
      `authentication.rs`'s `AUTHENTICATION_SPOTIFY_TOKEN` path -- reuses
      the OAuth token rather than needing a raw username/password). Needs
      a working post-handshake receive loop to deliver the result, which
      didn't exist before this either -- both landed together.
- [ ] **GCancellable plumbing.** None of ap.c's async calls (SRV resolve,
      TCP connect, handshake reads, receive loop reads) currently accept a
      cancellable -- hardcoded NULL throughout. Harmless in short-lived CLI
      usage (main.c's live-test path documents why), but a real
      use-after-free risk once this code is driven by a long-running app
      that might need to abort a stuck connection attempt.

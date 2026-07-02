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

**AP login needs a specific client_id and scope list, not any OAuth token.**
Confirmed the hard way: a token from `spotify-connect`'s dashboard-registered
Client ID (standard Web API scopes) was tested against
`AUTHENTICATION_SPOTIFY_TOKEN` login and rejected in ~100ms, no structured
error, just a closed connection. Checked against librespot's real source
(`src/main.rs`, the `--enable-oauth` path) rather than guessed at further:
their client uses `KEYMASTER_CLIENT_ID = "65b708073fc0480ea92a077233ca87bd"`
(`core/src/config.rs`) — Spotify's own internal client_id, not a
dashboard-registered one — with a specific 26-entry scope list
(`OAUTH_SCOPES` in `src/main.rs`). Redirect URI confirmed against
`librespot-oauth`'s own worked example
(`oauth/examples/oauth_sync.rs`): `http://127.0.0.1:8898/login`. All three
values transcribed and cross-checked (a full `diff` against the source, not
just eyeballed) into `apps/spotify-native/src/spotify/native_auth.h` —
see that file for the complete scope list and reasoning.

**`SystemInfo` has two `required` (proto2) fields — sending it empty is
invalid, not just incomplete.** Even after fixing the client_id/scopes
above, a live login attempt with a correctly-obtained token still failed
identically (~90ms silent close). Root cause was a second, independent bug
in the login message itself: `system_info` was written under field tag
`0x14`, but `authentication.proto`'s real field number is `0x32` — `0x14`
on `ClientResponseEncrypted` is actually `account_creation`, a different
field entirely, and the wrong number was most likely a transcription slip
from `LoginCredentials.typ`, which genuinely is `0x14` two lines above it
in the same function. Separately, even the right field number wouldn't
have been enough: `SystemInfo.cpu_family` and `SystemInfo.os` are both
`required`, and the message was sent completely empty. A strict proto2
parser is entitled to hard-reject a message missing required fields rather
than respond gracefully, which matches the observed failure signature
exactly. Fixed against librespot's own real construction
(`core/src/connection/mod.rs`'s `authenticate()`), not just the schema in
isolation — `cpu_family=CPU_X86_64(0x2)`, `os=OS_LINUX(0x5)`, plus
`system_information_string` and `device_id` for parity with what a real
client actually sends. The test that was supposed to catch this
(`test_login_encoding.c`) had re-implemented the same wrong field number
independently and asserted against itself, so it passed the whole time —
rewritten to check against the schema's real values instead of mirroring
whatever the implementation happened to do.

## Open items

- [x] Hand-roll the `keyexchange.proto` messages in C — `protobuf_min.c`,
      tested in `tests/test_protobuf.c`
- [x] HMAC-SHA1 key expansion — `handshake_crypto.c` (`hs_compute_keys`)
- [x] Full handshake wired into `ap.c` (`perform_handshake`) — ported from
      `core/src/connection/handshake.rs`, using the verified constants in
      `handshake_constants.h` and the real Shannon cipher
- [x] **Handshake confirmed against a live Spotify server.** DH exchange,
      RSA signature verification, and HMAC key derivation all checked out.
- [x] **native_auth wired up and confirmed reaching Spotify's real consent
      screen** (Spotify's own "Spotify for Desktop" branded page rendered,
      confirming the keymaster client_id is genuinely recognized server-side).
- [x] **Two login-message bugs found and fixed**: wrong `system_info` field
      number (`0x14` instead of `0x32`), and `SystemInfo`'s two required
      fields sent empty. See the finding above.
- [ ] **Next live run is the real test** of whether the fixed message
      actually gets accepted — everything above narrows the search space
      but a passing offline test only proves the message now matches the
      schema, not that Spotify's server accepts it.
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

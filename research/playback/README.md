# Playback research: audio key exchange, CDN fetch + decrypt

**Upstream reference:** `librespot-org/librespot`
- `core/src/audio_key.rs` — key request/response framing (AP/Mercury-based)
- `core/src/spclient.rs` — the actual HTTPS API for metadata + CDN URLs
- `core/src/apresolve.rs` — dynamic host resolution for spclient/dealer/AP
- `core/src/login5.rs` — mints the bearer token spclient's HTTPS calls need
- `protocol/proto/spotify/clienttoken/v0/clienttoken_http.proto`
- `protocol/proto/spotify/login5/v3/login5.proto`
- `protocol/proto/storage-resolve.proto`

## The real architecture (found while building this, not assumed upfront)

The original scoping here ("Audio key exchange, CDN fetch + decrypt") was
too narrow — that's real, but it's the *last* step of a longer chain, not
the whole thing. Discovered by reading `spclient.rs` directly rather than
guessing: modern Spotify resolves **track metadata and CDN URLs through a
separate HTTPS API (`spclient`)**, not through Mercury. Getting a CDN URL
for a file requires, in order:

```
AP login (already working)
    │
    ▼  APWelcome.reusable_auth_credentials (0x28) + type (0x1e)
    │  NOT the original native_auth OAuth token — a different credential
    │  APWelcome hands back specifically for this purpose
    ▼
client-token exchange (clienttoken.spotify.com)
    │  identifies which client is asking; tolerated as optional for some
    │  spclient endpoints, but confirmed REQUIRED for login5 itself
    │  (spclient.rs propagates failure with `?` on that specific path)
    ▼
login5 exchange (login5.spotify.com)
    │  StoredCredential { username, reusable_auth_credentials } → mints a
    │  short-lived Bearer token — THIS is what spclient's Authorization
    │  header actually wants, confirmed by reading request_with_options()
    │  directly rather than assumed
    ▼
spclient HTTPS API (spclient.wg.spotify.com, or apresolve-resolved)
    │  get_track_metadata, get_audio_storage (→ CDN URLs)
    ▼
audio_key.c (AP/Mercury-based, unrelated to the HTTPS chain above)
    ▼
cdn.c (HTTPS Range fetch + AES-CTR decrypt, already implemented)
```

## Status

- `spotify/native_auth.c` → `spotify/ap.c` (AP login) — ✅ confirmed
  working against a real server.
- `spotify/ap.c`'s APWelcome handling — ✅ now captures
  `reusable_auth_credentials`/`_type`, exposed via
  `spotifygtk_ap_session_get_reusable_creds()`. Found and fixed a
  transcription bug while adding this: `canonical_username` was read from
  field `0x14`, which is actually `account_type_logged_in` (an enum) —
  the real field is `0xa`. Login had been succeeding the whole time; only
  the username extraction was silently broken, which is exactly why no
  username ever printed in earlier test runs.
- `spotify/clienttoken.c` — ✅ implemented, and confirmed the request
  itself is now well-formed against a live server (HTTP 400 → HTTP 200,
  see the ConnectivitySdkData finding above). Uses librespot's own
  `client_version` (`"1.2.52.442"`, `core/src/version.rs`) and the
  keymaster `client_id`. Treats a genuine `ChallengesResponse` (HashCash/
  JS-eval/HMAC proof-of-work) as a soft failure rather than solved,
  matching librespot's own tolerance for this endpoint — distinguished
  from a malformed-request error via dedicated diagnostics rather than
  lumped together as "unknown failure."
- `spotify/login5.c` — ✅ implemented: builds `LoginRequest` with
  `StoredCredential` from the AP session's captured reusable credential.
  Requires a client-token (unlike some other spclient endpoints).
- `spotify/spclient.c` — 🟡 implemented for `get_audio_storage` only
  (`get_track_metadata` not yet started — no real file_id to test against
  until that exists). Host resolution uses librespot's own hardcoded
  fallback (`spclient.wg.spotify.com:443`) directly rather than
  implementing the full `apresolve` HTTPS flow — a deliberate, clearly-
  scoped simplification for a first working version, not an oversight.
- `spotify/audio_key.c` — ✅ request/response plumbing complete (unchanged,
  AP/Mercury-based, independent of the HTTPS chain above).
- `spotify/cdn.c` — 🟡 HTTPS Range fetch + AES-CTR decrypt implemented;
  IV seed and possible stream header offset still unconfirmed (see below).

**Client-token needed a real `ConnectivitySdkData` submessage, not an
optional nicety.** First live run against the real endpoint returned
HTTP 400 with a zero-length body — not a graceful `ChallengesResponse`,
which is what the earlier "tolerated as optional" reading from
`spclient.rs` would have predicted. The diagnostics added specifically to
distinguish those two cases (real challenge vs. genuinely broken request)
were what made this immediately diagnosable instead of another guess.
Root cause: `ClientDataRequest.connectivity_sdk_data` (the oneof `data`
field) needs to actually be populated — confirmed against `go-librespot`'s
`connectivity.proto` for the message shape and librespot's
`spclient.rs::mut_desktop_linux()` branch for what a real Linux client
sends (device_id, OS name/version, CPU architecture via `uname(2)`). Fixed
in `clienttoken.c`; the same `device_id` is now reused consistently across
both the client-token request and login5's `ClientInfo.device_id`.

`clienttoken.c`, `login5.c`, and `spclient.c` are otherwise still
unconfirmed against real services beyond this one fix — the next live run
is what actually proves the full chain (client-token → login5 → a real
bearer token) end to end.

## Open items

- [ ] Confirm the client-token → login5 chain actually succeeds against
      real services (main.c's live test now exercises this automatically)
- [ ] Implement `apresolve` properly instead of the hardcoded
      `spclient.wg.spotify.com` fallback (works per librespot's own code,
      but is a simplification worth closing out)
- [ ] `spclient_get_track_metadata` — needed to get a real file_id to test
      `get_audio_storage` against; nothing currently produces one
- [ ] Confirm CTR IV seed against librespot's actual AES setup for cdn.c
- [ ] Confirm (or rule out) a stream header offset by comparing a captured
      encrypted chunk's first bytes against expected Ogg magic bytes
      (`OggS`) at various candidate offsets
- [ ] Wire audio_key.c + cdn.c + decoder.c + output.c into an actual
      end-to-end playback path once a real file_id is obtainable

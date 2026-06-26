# Playback research: audio key exchange, CDN fetch + decrypt

**Upstream reference:** `librespot-org/librespot`
- `core/src/audio_key.rs` — key request/response framing
- `metadata/src/audio/` — CDN URL resolution, file format selection

## Status

`apps/spotify-native/src/spotify/audio_key.c` — request/response plumbing
complete (packet layout is simple request/response, low risk to have
implemented in full already).

`apps/spotify-native/src/spotify/cdn.c` — HTTPS Range fetch + AES-128-CTR
decrypt both implemented for real. **Open question:** the IV/counter
seed value and whether there's an unencrypted header before the encrypted
Ogg stream begins (`STREAM_HEADER_OFFSET` in cdn.c, currently 0) — needs
confirming against a real captured/decrypted file before relying on it.

## Open items

- [ ] Confirm CTR IV seed against librespot's actual AES setup for this
- [ ] Confirm (or rule out) a stream header offset by comparing a captured
      encrypted chunk's first bytes against expected Ogg magic bytes
      (`OggS`) at various candidate offsets

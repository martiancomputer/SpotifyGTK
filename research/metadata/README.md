# Metadata research: Mercury pub/sub

**Upstream reference:** `librespot-org/librespot` — `core/src/mercury/`
Also check `librespot-java` (devgianlu) where it's ahead of the Rust impl.

## Status

`apps/spotify-native/src/spotify/mercury.c` has framing implemented but
**unverified against live traffic** — field widths (sequence number,
flags, part count/length encoding) follow commonly-documented community
reverse-engineering, not a fresh capture against this project's own AP
connection (since the AP handshake itself isn't live yet — see research/auth/).

## Open items

- [ ] Once `ap.c`'s handshake is real, capture an actual Mercury exchange
      and confirm field widths against this implementation
- [ ] Cross-reference librespot-java's Mercury implementation for anything
      that's changed since the Rust version was last updated

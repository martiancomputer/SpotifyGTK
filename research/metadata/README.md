# Metadata research: Mercury pub/sub

**Upstream reference:** `librespot-org/librespot` — `core/src/mercury/`
Also check `librespot-java` (devgianlu) where it's ahead of the Rust impl.

## Status

`apps/spotify-native/src/spotify/mercury.c` has framing implemented but
**has never run at all.** Not merely unverified against live traffic:
`spotifygtk_mercury_new()` is called from nowhere in the tree, and its only
would-be caller, `spotify/connect.c`, is never instantiated either. Field widths
(sequence number, flags, part count/length encoding) follow commonly-documented
community reverse-engineering rather than a capture.

The AP handshake *is* live now, so the blocker in the note below is gone — what
remains is that nothing has been wired to drive Mercury over it.

**A free source of real frames is already arriving and being discarded.** Every
session logs `ap.c: no handler registered for incoming cmd=0xb5` with a payload
of a few hundred bytes; `0xb5` is `AP_CMD_MERCURY_EVENT`. The server pushes
well-formed Mercury frames unprompted, so the parser can be validated against
real data before any request is sent — which also separates "our framing is
wrong" from "that endpoint is wrong", an ambiguity that has blocked the
collection work (see `spotify/collection.h`).

## Open items

- [ ] Register a handler for `AP_CMD_MERCURY_EVENT` (0xb5) and parse the frames
      the AP already sends, to confirm field widths against real data
- [ ] Then drive an outbound request, so a failure is attributable to the
      endpoint rather than to the transport
- [ ] Cross-reference librespot-java's Mercury implementation for anything
      that's changed since the Rust version was last updated

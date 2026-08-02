# Connect research: Spirc/dealer protocol, device registration, ad-insertion

**Upstream reference:** `librespot-org/librespot` — `connect/src/`

## Why this matters more than it looks like it should

Per the project's Legal & ethical approach (see root README): faithfully
implementing ad-insertion and feature-gating events for free-tier accounts
is the difference between "an alternative client" and "an accidental
ad-stripper." This is tracked here specifically so it doesn't get
deprioritized as just plumbing.

**Note (2026-08-02): free-tier playback is not reachable at all**, so this is a
standing commitment rather than pending work. A free account is refused an audio
key for every file a track offers — OGG_VORBIS 320/160/96 and AAC_24 alike, each
within ~200ms — so the gate is the audio-key exchange itself, not a format or a
bitrate. Nothing here can play a note on a free account, which means there is
currently no ad to insert. If that ever changes, this is the work that has to
land alongside it, not after.

## Open items

- [ ] Device-state announcement payload — currently a placeholder JSON-ish
      string in `connect.c`, needs the real protobuf schema
- [ ] Ad-insertion / feature-state event handling — not yet researched at
      all; start by finding where librespot's `connect/src/` surfaces these
      events (if it does -- it may intentionally not implement them, which
      would itself be useful to know and would mean we need a different
      reference)
- [ ] Remote command handling (play/pause/seek/transfer from another
      client) — `connect.c`'s "remote-command" signal exists but nothing
      consumes it yet

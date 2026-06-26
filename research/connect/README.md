# Connect research: Spirc/dealer protocol, device registration, ad-insertion

**Upstream reference:** `librespot-org/librespot` — `connect/src/`

## Why this matters more than it looks like it should

Per the project's Legal & ethical approach (see root README): faithfully
implementing ad-insertion and feature-gating events for free-tier accounts
is the difference between "an alternative client" and "an accidental
ad-stripper." This is tracked here specifically so it doesn't get
deprioritized as just plumbing.

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

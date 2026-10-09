# Project guide for coding agents

## Overview

SpotifyGTK is an independent native Spotify client for Linux and Windows,
written in C with GTK4 and libadwaita. The native player implements sign-in,
catalog browsing, streaming playback, Spotify Connect, and optional local-file
playback. Spotify streaming requires a Premium account. Do not bypass account,
licensing, or service access checks.

The interface includes personalised Home shelves, Search, Library, Liked Songs,
album and artist pages, playlists, a Now Playing panel, and audio settings.
Local favorites and device-only playlists are separate from Spotify's data;
local playlist overlays must not be uploaded as Spotify tracks.

## Repository map

- `apps/spotify-native/`: main Meson project and native player.
  - `src/spotify/`: authentication, protocol, catalog, session, and Connect code.
  - `src/audio/`: decoding, audio processing, and platform output backends.
  - `src/ui/`: GTK widgets, navigation, page models, artwork, and styling.
  - `tests/`: protocol, cache, audio, and UI regression tests.
  - `data/`: application resources and desktop integration.
- `apps/spotify-native-windows/`: Windows toolchain and packaging support for
  the shared native source tree; do not create a separate fork of the player.
- `apps/spotify-connect/`: separate HTTP/JSON control client, not the player.
- `research/`: protocol findings, architecture, and legal context.
- `packaging/`: distribution packaging support.
- `README.md`, `LICENSE`, and `THIRD_PARTY_LICENSES`: public project guidance,
  licensing, and upstream attribution.

## Build and verification

Run these commands from the repository root for a new stable build:

```sh
meson setup apps/spotify-native/build apps/spotify-native \
  --native-file apps/spotify-native/build-profiles/stable.ini
meson compile -C apps/spotify-native/build
meson test -C apps/spotify-native/build --print-errorlogs
```

Reuse an existing configured build directory when appropriate. Consult the
main README for dependencies, the nightly profile, and GTK version requirements.
Use `-Denable_gui=false` for an engine-only build. Windows build instructions
are in `apps/spotify-native-windows/README.md`.

Add focused regression coverage for behavioral changes and run the relevant
tests, followed by broader coverage when feasible. UI tests need a working
display and renderer; report environmental failures separately from product
failures. Check visual changes in light and dark themes and at narrow and wide
window sizes. Do not claim live performance or memory improvements without
measurements. Documentation-only changes do not require rebuilding the app.

## Implementation principles

- Keep network requests, decoding, and expensive filesystem work off the GTK
  main thread. Preserve cancellation and stale-response guards during navigation.
- Make GLib/GObject ownership explicit. Disconnect callbacks and remove timers
  before disposing their owners; release temporary artwork when its view closes.
- Keep metadata caches account-scoped and bounded. Do not retain decoded artwork
  for every item in a collection. Preserve offscreen widget and texture eviction.
- Prefer shared media-card widgets and theme-aware styling over page-specific
  duplicates. Keep title and subtitle bounds aligned with the cover-art column.
- Preserve playlist order, local track identities, playback queues, and the
  separation between device-only data and server-owned data.
- Keep platform differences in the shared source behind appropriate abstractions.
- Keep optional diagnostics opt-in; normal builds should not acquire recurring
  profiling overhead. Sanitize diagnostic output before sharing it.
- Preserve unrelated working-tree edits. Never use destructive Git operations
  or rewrite published history without explicit authorization.
- Retain required license notices and document upstream sources when porting code.

## Commit and push privacy rules

1. Never commit personally identifying or machine-specific information: real
   usernames, home-directory or workspace paths, hostnames, device names,
   account identifiers, or personally identifying email addresses. Use neutral
   placeholders such as `<user>`, `<host>`, `<workspace>`, `<device>`, and
   `<account-id>`, or runtime-derived paths.
2. Never commit secrets or credentials, including passwords, API tokens, private
   keys, cookies, authentication headers, recovery codes, secret environment
   values, or private account/project identifiers. Use secure configuration or
   ask the user to supply secrets manually.
3. Do not commit unnecessary identifying network data, including private IP
   addresses, MAC addresses, or device serial numbers. Use sanitized examples
   unless an exact value is technically required and explicitly approved.
4. Treat local notes, scratch files, logs, packet captures, authentication dumps,
   browser/session data, and machine-specific instructions as private. Keep
   appropriate local-only patterns ignored. Do not automatically ignore normal
   repository documentation such as `README.md`.
5. Before every commit, review `git diff --cached` and scan staged changes for
   sensitive values. Sanitize questionable content before committing. Stage
   only intended project files, not private investigation artifacts.
6. Before every push, inspect the entire outgoing commit range for sensitive
   content, not just the current working tree. Do not push if sensitive data is
   found; resolve it with the user before changing published history.
7. Do not commit `.env`, credential files, key files, or secret configuration
   unless explicitly instructed and confirmed safe.
8. Use sanitized examples in documentation, logs, commit messages, and notes.
   Do not copy private local paths or identifiers into public project content.

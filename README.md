# SpotifyGTK

A native Spotify client project for Linux (Windows planned), written entirely
in **C** — zero Rust, zero Electron. Built for raw performance and long-term
stability rather than convenience.

![License](https://img.shields.io/github/license/martiancomputer/SpotifyGTK)
![CI](https://github.com/martiancomputer/SpotifyGTK/actions/workflows/build.yml/badge.svg)

---

## Project layout

This is a monorepo containing **two independent apps** plus the research
behind them. Each app has its own `meson.build` and is built separately.

```
SpotifyGTK/
├── apps/
│   ├── spotify-connect/      Web API control client (functional today)
│   └── spotify-native/       Standalone engine: protocol + audio (in progress)
│
├── research/                 Protocol reverse-engineering notes, by area
│   ├── auth/                 AP handshake, Diffie-Hellman, Shannon cipher
│   ├── metadata/              Mercury pub/sub
│   ├── connect/               Spirc/dealer, device registration, ad-insertion
│   └── playback/              Audio key exchange, CDN fetch + decrypt
│
├── docs/                      General project documentation
└── THIRD_PARTY_LICENSES       Attribution for ported/referenced code
```

### Why two apps

The Web API control surface (`auth.c`, `api.c`, search, playback *control*)
and the standalone protocol/audio engine (`spotify/`, `audio/`) turned out to
have very different hardware requirements. The former is just HTTP + JSON —
light enough to run on an MCU or any resource-constrained board, and needs
no audio decode libraries, no PulseAudio/ALSA, nothing audio-related at all.
The latter needs a real decode pipeline and benefits from GPU acceleration —
a desktop-class target. Splitting them let each app's dependency list shrink
to only what it actually uses, instead of one binary linking everything.

---

## `apps/spotify-connect`

Controls an **existing** Spotify Connect device (phone, desktop app, speaker)
via the official Web API. Requires Spotify Premium (Spotify's Web Playback
endpoints are Premium-gated). Doesn't play audio itself — it tells an already-
active device what to do. This is the lightweight, MCU-capable half of the
project, and it's the part that's functional today.

**Stack:** GTK4 / libadwaita UI, libsoup3, json-glib, OAuth 2.0 PKCE.

| Module | Status |
|---|---|
| OAuth 2.0 PKCE | ✅ Functional |
| Web API wrapper | ✅ Functional |
| Search → playback control | ✅ Functional |
| Image cache (LRU + disk + libjpeg-turbo/stb_image) | ✅ Functional |
| Diagnostic logging (status codes, request/response tracing) | ✅ Functional |

### Building

```bash
cd apps/spotify-connect
meson setup build --native-file build-profiles/stable.ini
# or: meson setup build --native-file build-profiles/nightly.ini
ninja -C build
```

### Authentication

OAuth 2.0 Authorization Code + PKCE — no client secret ever stored on disk.

1. Register an app at [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard)
2. Add redirect URI: `http://127.0.0.1:8888/callback`
3. Copy your **Client ID**, then:

```bash
export SPOTIFY_CLIENT_ID="your_client_id_here"
./build/src/spotify-connect
```

Your browser opens for login; the token is captured on `127.0.0.1:8888` and
stored via **libsecret** (or `~/.config/spotifygtk/tokens`, `chmod 600`, if
libsecret isn't available).

### Install system-wide

```bash
sudo ninja -C build install
```

### Two release tracks: Stable & Nightly

| | **Stable** | **Nightly** |
|---|---|---|
| GTK | 4.0+ | 4.14+ |
| Image decode | stb_image / libjpeg-turbo | + VA-API hardware decode |
| Texture upload | `gdk_memory_texture_new()` | `GdkDmabufTexture` (zero-copy) |
| Targets | Ubuntu 22.04+, RHEL 9, Debian 12 | Arch, Fedora, Ubuntu 24.04+ |

### Prerequisites

**Ubuntu / Debian (24.04+)**
```bash
sudo apt install \
  meson ninja-build pkg-config \
  libgtk-4-dev libadwaita-1-dev \
  libsoup-3.0-dev libjson-glib-dev \
  libsecret-1-dev libglib2.0-dev \
  libjpeg-turbo8-dev
# Nightly only:
sudo apt install libva-dev libva-drm2
```

**Fedora / RHEL**
```bash
sudo dnf install \
  meson ninja-build pkg-config \
  gtk4-devel libadwaita-devel \
  libsoup3-devel json-glib-devel \
  libsecret-devel glib2-devel \
  libjpeg-turbo-devel
```

**Arch Linux**
```bash
sudo pacman -S meson gtk4 libadwaita libsoup3 json-glib libsecret \
  libjpeg-turbo libva
```

---

## `apps/spotify-native`

The original objective: a fully standalone client that's its own Spotify
Connect device, with its own audio decode and output pipeline — no other
Spotify client needs to be running. GPU-accelerated where hardware allows
(VA-API hardware JPEG decode planned; Vulkan compositing not started).

**Currently a development harness, not a real client.** There's no UI, no
AP login flow, and no playback yet — see Project status below for the exact
breakdown. What it does have: a real Shannon cipher self-test you can
actually run.

### Building

```bash
cd apps/spotify-native
meson setup build --native-file build-profiles/stable.ini
# or: meson setup build --native-file build-profiles/nightly.ini
ninja -C build
./build/src/spotify-native-harness
```

### Legal & ethical approach

This part of the project reimplements Spotify's binary protocol, which is
worth being direct about. Two real-world precedents shape how this is built:

- **GitHub took down `whoeevee/EeveeSpotify` in August 2025** — but notably,
  GitHub *rejected* Spotify's DMCA anti-circumvention (§1201) claim as
  insufficiently supported. The takedown succeeded on a different ground:
  GitHub's own Acceptable Use Policy against software that patches an official
  app binary to bypass Premium/licensing checks. EeveeSpotify modified
  Spotify's actual compiled app and unlocked paid features for free.
- **`librespot`** (MIT-licensed, ~6.7k stars) has done a clean-room
  reimplementation of this same protocol — AP connection, Shannon cipher,
  Mercury, audio key exchange, CDN decrypt — continuously since 2015, with no
  DMCA action against it, while requiring users' own real accounts.

The operating principles here follow directly from that contrast:

1. **Clean-room only.** No decompiling or copying Spotify's actual app
   binaries or extracted assets.
2. **Requires a real, paid Spotify account.** Nothing here bypasses a Premium
   or licensing check — that's the specific thing that's actually been
   enforced against, separate from the protocol-reimplementation question.
3. **Playback only, never a downloader.** This is a client for content you're
   already licensed to stream, not a bulk-export/DRM-stripping tool.
4. **Faithful, not selectively faithful.** Ad-insertion and feature-state
   events for free-tier accounts get implemented along with everything else
   (tracked in `research/connect/`) — the goal is an alternative client, not
   an accidental ad-stripper that happens to also play music.
5. **`librespot`'s MIT-licensed work is primary reference material**, properly
   attributed in `THIRD_PARTY_LICENSES`, rather than starting from raw packet
   captures. They already did the clean-room work; their license permits
   building on it.
6. **Same disclaimer librespot carries:** using this to connect to Spotify's
   service is probably against their Terms of Service. Use at your own risk.
   This is a Terms-of-Service question, separate from the copyright/DMCA
   question above — account suspension is the realistic consequence, accepted
   knowingly. This isn't legal advice; the DMCA §1201 question for protocol
   clients specifically hasn't been tested in court.

### Project status

Being direct about what's implemented vs. scaffolded, because a stream cipher
or DH handshake that *looks* done but is subtly wrong is worse than an honest
gap:

| Module | Status |
|---|---|
| Shannon cipher (`spotify/shannon.c`) | ✅ Real algorithm, ported from the `shannon` Rust crate (MIT) librespot depends on. Self-test in `main.c` passes. |
| Ogg/Vorbis decoder (`audio/decoder.c`) | ✅ Functional |
| Audio output: PulseAudio, ALSA | ✅ Functional |
| Audio output: PipeWire | 🟡 Implemented, needs validation against a running PipeWire instance |
| Audio output: WASAPI (Windows) | ⬜ Stub only — Windows port hasn't started |
| AP handshake (`spotify/ap.c`, `dh.c`, `handshake_crypto.c`, `protobuf_min.c`) | ✅ Confirmed working against a real Spotify server — DH exchange, RSA server-signature verification, and HMAC-SHA1 key derivation all checked out. |
| AP login (`spotify/ap.c`, `spotify/native_auth.c`) | 🟡 Two real bugs found and fixed on the way here: wrong client_id/scopes (needed Spotify's internal keymaster client_id, not a Web API token — see `research/auth/`), then a wrong protobuf field number plus two missing required fields in the login message itself, caught because a live server rejected it, not by the tests that existed at the time. Both fixed; not yet confirmed accepted by a live server. |
| Mercury protocol (`spotify/mercury.c`) | 🟡 Framing implemented, unverified against live traffic |
| Audio key exchange (`spotify/audio_key.c`) | ✅ Request/response plumbing complete |
| CDN fetch + AES-CTR decrypt (`spotify/cdn.c`) | 🟡 HTTPS Range + decrypt real, IV seed pending confirmation against a captured stream |
| Spotify Connect registration (`spotify/connect.c`) | 🟡 Mercury subscription real, device-state payload pending real protobuf schema; ad-insertion events not yet researched |
| Image cache VA-API hardware decode | 🟡 Probe works, decode path stubbed (lives in `spotify-connect`, shared concept) |
| Vulkan compositing | ⬜ Not started |
| UI | ⬜ None yet — `main.c` is a CLI development harness |

### Audio backend tracks

```bash
meson setup build --native-file build-profiles/stable.ini   # Pulse/ALSA
meson setup build --native-file build-profiles/nightly.ini  # + PipeWire
```

### Prerequisites

**Ubuntu / Debian (24.04+)**
```bash
sudo apt install \
  meson ninja-build pkg-config \
  libglib2.0-dev libsoup-3.0-dev \
  libogg-dev libvorbis-dev \
  libssl-dev libpulse-dev libasound2-dev
# Nightly only:
sudo apt install libpipewire-0.3-dev
```

**Fedora / RHEL**
```bash
sudo dnf install \
  meson ninja-build pkg-config \
  glib2-devel libsoup3-devel \
  libogg-devel libvorbis-devel \
  openssl-devel pulseaudio-libs-devel alsa-lib-devel
```

**Arch Linux**
```bash
sudo pacman -S meson glib2 libsoup3 libogg libvorbis openssl libpulse alsa-lib pipewire
```

---

## `research/`

Protocol documentation organized by area, each citing its upstream reference.
See each subfolder's own README for confirmed findings and open items —
`research/auth/` in particular already has real, verified material (the DH
parameters, RSA server key, and protobuf schema) waiting to be wired into
`ap.c`.

Also referenced: `librespot-org/spotify-connect-resources` (protocol data
dumps) and `librespot-java` (devgianlu) for areas where it's ahead of the Rust
implementation.

Every finding that gets ported into `apps/spotify-native/src/spotify/` should
note which upstream file it's translated from — see `THIRD_PARTY_LICENSES`.

---

## Core principles (apply to both apps)

- **No dependency younger than 5 years** for any critical path
- **Every external library is optional**, with a pure-C fallback
- **Runtime probing over compile-time feature gates** — the binary works
  everywhere, it just silently takes the best available path

## Architecture (`spotify-native`)

```
┌──────────────────────────────────────┐
│         spotify-native-harness        │
│         (src/main.c, CLI only)        │
└───────────────┬───────────────────────┘
                │
┌───────────────▼───────────────────────┐
│           Protocol layer               │
│           (src/spotify/)               │
│   Shannon cipher (real), AP framing,   │
│   Mercury pub/sub, audio key exchange, │
│   CDN chunk fetching                   │
└───────────────┬───────────────────────┘
                │
┌───────────────▼───────────────────────┐
│           Audio engine                 │
│           (src/audio/)                 │
│   Ogg/Vorbis decode,                   │
│   PipeWire/Pulse/ALSA/WASAPI output    │
└────────────────────────────────────────┘
```

Image cache (3-tier decode: VA-API → libjpeg-turbo → stb_image, LRU + disk
cache, worker thread pool) currently lives in `spotify-connect` since that's
where the only UI is — `spotify-native` will need its own copy or a shared
lib once it has one too.

---

## Running Tests

```bash
cd apps/spotify-connect && meson test -C build --print-errorlogs
cd apps/spotify-native  && meson test -C build --print-errorlogs
```

## Contributing

1. Fork, branch off `main`
2. Match existing style (2-space indent, GLib naming conventions)
3. Run `cppcheck` on the app you touched, excluding `vendor/`
4. If touching `spotify/shannon.c` or `spotify/ap.c`: cite your source —
   `research/auth/` should already have the upstream reference; if porting
   new logic from `librespot`, add the attribution there first

### Roadmap

- [x] Reorganize into `apps/spotify-connect/` + `apps/spotify-native/`
- [x] Port real Shannon cipher from librespot's `shannon` crate
- [x] Document real DH params, RSA server key, keyexchange.proto schema
- [x] Hand-roll `keyexchange.proto` encoder, wire DH handshake into `ap.c`
- [x] Test the handshake against a live Spotify server
- [ ] Confirm AP login succeeds end-to-end with the corrected native_auth token
- [ ] Mercury protocol validation against live traffic
- [ ] Ad-insertion / feature-state event handling (`research/connect/`)
- [ ] VA-API hardware JPEG decode path (probe works, decode pending)
- [ ] Vulkan compositing, UI for `spotify-native`
- [ ] MPRIS2 D-Bus interface
- [ ] Flatpak packaging
- [ ] Windows port (WASAPI backend, MSYS2/MinGW build)

---

## License

**GNU General Public License v3.0** — see [LICENSE](LICENSE).

Portions of `apps/spotify-native` are ported from or reference
[`librespot`](https://github.com/librespot-org/librespot) and the `shannon`
crate it depends on (both MIT License) — see `THIRD_PARTY_LICENSES` for full
attribution.

SpotifyGTK is an independent open-source project, not affiliated with or
endorsed by Spotify AB. "Spotify" is a trademark of Spotify AB. Connecting to
Spotify's service using this software is likely outside Spotify's Terms of
Service — use at your own risk.

# SpotifyGTK

A native Spotify client project for Linux (Windows planned), written entirely
in **C** — zero Rust, zero Electron. Built for raw performance and long-term
stability rather than convenience.

![License](https://img.shields.io/github/license/martiancomputer/SpotifyGTK)
![CI](https://github.com/martiancomputer/SpotifyGTK/actions/workflows/build.yml/badge.svg)

---

## Project layout

This is now a monorepo containing **two complementary apps** plus the research
behind them:

```
SpotifyGTK/
├── apps/
│   ├── spotifygtk-remote/    Lightweight Web API control client (works today)
│   └── spotifygtk-native/    Standalone client, own audio engine, GPU-accelerated
│
├── research/                 Protocol reverse-engineering notes, organized by area
│   ├── auth/                 AP handshake, Shannon cipher
│   ├── metadata/              Mercury pub/sub
│   ├── connect/               Spirc/dealer, device registration
│   └── playback/              Audio key exchange, CDN fetch + decrypt
│
└── docs/                      General project documentation
```

> **Migration note:** the code today still lives in a flat `src/` layout from
> before this split. It's being reorganized into `apps/spotifygtk-remote/` —
> functionally nothing changes, it's the same Web-API-based client described
> below, just relocating to make room for `spotifygtk-native` alongside it.

### Why two apps

The Web API control surface (`api.c`, OAuth, search, playback *control*) and
the standalone protocol/audio engine (`spotify/`, `audio/`) turned out to have
very different hardware requirements. The former is just HTTP + JSON — light
enough to run on an MCU or any resource-constrained board. The latter needs a
real audio decode pipeline and benefits from Vulkan/VA-API GPU acceleration —
that's a desktop-class target. Splitting them lets each app be built and
optimized for what it actually is, instead of one binary trying to be both.

---

## `apps/spotifygtk-remote`

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

---

## `apps/spotifygtk-native`

The original objective: a fully standalone client that's its own Spotify
Connect device, with its own audio decode and output pipeline — no other
Spotify client needs to be running. GPU-accelerated where hardware allows
(VA-API hardware JPEG decode, planned Vulkan compositing).

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
4. **`librespot`'s MIT-licensed work is primary reference material**, properly
   attributed in `THIRD_PARTY_LICENSES`, rather than starting from raw packet
   captures. They already did the clean-room work; their license permits
   building on it.
5. **Same disclaimer librespot carries:** using this to connect to Spotify's
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
| GTK4/libadwaita UI shell | ✅ Functional |
| Ogg/Vorbis decoder (`audio/decoder.c`) | ✅ Functional |
| Audio output: PulseAudio, ALSA | ✅ Functional |
| Audio output: PipeWire | 🟡 Implemented, needs validation against a running PipeWire instance |
| Audio output: WASAPI (Windows) | ⬜ Stub only — Windows port hasn't started |
| AP connection + packet framing (`spotify/ap.c`) | 🟡 TCP/SRV resolution real, DH handshake not implemented |
| Shannon cipher (`spotify/shannon.c`) | 🟡 Key schedule/buffering real, round function pending port from librespot reference (see `research/auth/`) |
| Mercury protocol (`spotify/mercury.c`) | 🟡 Framing implemented, unverified against live traffic |
| Audio key exchange (`spotify/audio_key.c`) | ✅ Request/response plumbing complete |
| CDN fetch + AES-CTR decrypt (`spotify/cdn.c`) | 🟡 HTTPS Range + decrypt real, IV seed pending confirmation against a captured stream |
| Spotify Connect registration (`spotify/connect.c`) | 🟡 Mercury subscription real, device-state payload pending real protobuf schema |
| Image cache VA-API hardware decode | 🟡 Probe works, decode path stubbed |
| Vulkan compositing | ⬜ Not started |

### Two release tracks: Stable & Nightly

Modeled on Debian stable/sid and Arch [stable]/[testing] — one codebase, two
build profiles, selected at `meson setup` time:

| | **Stable** | **Nightly** |
|---|---|---|
| GTK | 4.0+ | 4.14+ |
| Image decode | stb_image / libjpeg-turbo | + VA-API hardware decode |
| Audio output | PulseAudio / ALSA | + native PipeWire |
| Texture upload | `gdk_memory_texture_new()` | `GdkDmabufTexture` (zero-copy) |
| Targets | Ubuntu 22.04+, RHEL 9, Debian 12 | Arch, Fedora, Ubuntu 24.04+ |
| Guarantee | Works out of the box, no debugging | Best possible performance |

```bash
meson setup build --native-file build-profiles/stable.ini
# or
meson setup build --native-file build-profiles/nightly.ini
```

---

## `research/`

Protocol documentation organized by area, each citing its upstream reference:

```
research/
├── auth/        AP handshake, Diffie-Hellman, Shannon cipher
│                 -> librespot: core/src/connection/, core/src/diffie_hellman.rs
├── metadata/     Mercury pub/sub
│                 -> librespot: core/src/mercury/
├── connect/      Spirc/dealer protocol, device registration
│                 -> librespot: connect/src/
└── playback/     Audio key exchange, CDN fetch + AES-CTR decrypt
                  -> librespot: metadata/src/audio/, core/src/audio_key.rs
```

Also referenced: `librespot-org/spotify-connect-resources` (protocol data
dumps) and `librespot-java` (devgianlu) for areas where it's ahead of the Rust
implementation.

Every finding here that gets ported into `apps/spotifygtk-native/src/spotify/`
should note which upstream file it's translated from.

---

## Core principles (apply to both apps)

- **No dependency younger than 5 years** for any critical path
- **Every external library is optional**, with a pure-C fallback
- **Runtime probing over compile-time feature gates** — the binary works
  everywhere, it just silently takes the best available path

## Architecture (`spotifygtk-native`)

```
┌──────────────────────────────────────────────────┐
│                  SpotifyGTK UI                    │
│         GTK4 / libadwaita (src/ui/)               │
└──────────┬───────────────────────┬────────────────┘
           │                       │
┌──────────▼──────────┐  ┌─────────▼──────────────┐
│   Web API client     │  │   Protocol layer        │
│   (src/api.c)         │  │   (src/spotify/)        │
│   OAuth2 PKCE         │  │   AP connection,        │
│   search/library/     │  │   Mercury pub/sub,      │
│   playback control    │  │   audio key exchange,   │
│   via api.spotify.com │  │   CDN chunk fetching     │
└──────────────────────┘  └─────────┬──────────────┘
                                     │
                          ┌──────────▼──────────────┐
                          │   Audio engine            │
                          │   (src/audio/)            │
                          │   Ogg/Vorbis decode,      │
                          │   PipeWire/Pulse/ALSA/    │
                          │   WASAPI output            │
                          └────────────────────────────┘

┌──────────────────────────────────────────────────┐
│   Image cache (src/image_cache.c)                  │
│   L1 in-memory LRU + L2 disk cache                 │
│   3-tier decode: VA-API → libjpeg-turbo → stb_image│
│   Worker thread pool, never blocks the GTK thread  │
└──────────────────────────────────────────────────┘
```

---

## Prerequisites

| Package | Stable | Nightly |
|---|---|---|
| GTK4 | 4.0+ | 4.14+ |
| libadwaita | 1.4+ | 1.4+ |
| libsoup3 | 3.4+ | 3.4+ |
| json-glib | 1.6+ | 1.6+ |
| libogg / libvorbis | 1.3+ | 1.3+ |
| OpenSSL | 3.0+ | 3.0+ |
| libjpeg-turbo | any | any |
| PulseAudio or ALSA | one required | one required |
| libsecret | optional | optional |
| libva + libva-drm | — | optional |
| libpipewire-0.3 | — | optional |

**Ubuntu / Debian (24.04+)**
```bash
sudo apt install \
  meson ninja-build pkg-config \
  libgtk-4-dev libadwaita-1-dev \
  libsoup-3.0-dev libjson-glib-dev \
  libsecret-1-dev libglib2.0-dev \
  libogg-dev libvorbis-dev \
  libssl-dev libjpeg-turbo8-dev \
  libpulse-dev libasound2-dev
# Nightly only:
sudo apt install libva-dev libva-drm2 libpipewire-0.3-dev
```

**Fedora / RHEL**
```bash
sudo dnf install \
  meson ninja-build pkg-config \
  gtk4-devel libadwaita-devel \
  libsoup3-devel json-glib-devel \
  libsecret-devel glib2-devel \
  libogg-devel libvorbis-devel \
  openssl-devel libjpeg-turbo-devel \
  pulseaudio-libs-devel alsa-lib-devel
```

**Arch Linux**
```bash
sudo pacman -S meson gtk4 libadwaita libsoup3 json-glib libsecret \
  libogg libvorbis openssl libjpeg-turbo libpulse alsa-lib \
  libva pipewire
```

---

## Building

```bash
git clone https://github.com/martiancomputer/SpotifyGTK.git
cd SpotifyGTK

meson setup build --native-file build-profiles/stable.ini
# or: meson setup build --native-file build-profiles/nightly.ini

ninja -C build
./build/src/spotifygtk
```

### Install system-wide

```bash
sudo ninja -C build install
```

---

## Authentication

OAuth 2.0 Authorization Code + PKCE — no client secret ever stored on disk.

1. Register an app at [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard)
2. Add redirect URI: `http://127.0.0.1:8888/callback`
3. Copy your **Client ID**, then:

```bash
export SPOTIFY_CLIENT_ID="your_client_id_here"
./build/src/spotifygtk
```

Your browser opens for login; the token is captured on `127.0.0.1:8888` and
stored via **libsecret** (or `~/.config/spotifygtk/tokens`, `chmod 600`, if
libsecret isn't available).

---

## Running Tests

```bash
meson test -C build --print-errorlogs
```

## Contributing

1. Fork, branch off `main`
2. Match existing style (2-space indent, GLib naming conventions)
3. Run `cppcheck src/ -i src/vendor` before submitting
4. If touching `spotify/shannon.c` or `spotify/ap.c`: cite your source —
   `research/auth/` should already have the upstream reference; if porting
   new logic from `librespot`, add the attribution there first

### Roadmap

- [ ] Reorganize `src/` into `apps/spotifygtk-remote/`
- [ ] Scaffold `apps/spotifygtk-native/`
- [ ] Port Shannon cipher + DH handshake from librespot (`research/auth/`)
- [ ] Mercury protocol validation against live traffic
- [ ] VA-API hardware JPEG decode path (probe works, decode pending)
- [ ] Vulkan compositing for `spotifygtk-native`
- [ ] MPRIS2 D-Bus interface
- [ ] Flatpak packaging
- [ ] Windows port (WASAPI backend, MSYS2/MinGW build)

---

## License

**GNU General Public License v3.0** — see [LICENSE](LICENSE).

Portions of `apps/spotifygtk-native` reference or port logic from
[`librespot`](https://github.com/librespot-org/librespot) (MIT License) —
see `THIRD_PARTY_LICENSES` once that porting begins.

SpotifyGTK is an independent open-source project, not affiliated with or
endorsed by Spotify AB. "Spotify" is a trademark of Spotify AB. Connecting to
Spotify's service using this software is likely outside Spotify's Terms of
Service — use at your own risk.

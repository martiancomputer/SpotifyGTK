# SpotifyGTK

A native **GTK4 / libadwaita** Spotify client for Linux (Windows port planned) —
written entirely in **C**, with zero Rust dependencies. Built for raw performance
and long-term stability rather than convenience.

![License](https://img.shields.io/github/license/martiancomputer/SpotifyGTK)
![CI](https://github.com/martiancomputer/SpotifyGTK/actions/workflows/build.yml/badge.svg)

---

## Why this exists

The official Spotify client is Electron — a full Chromium + V8 + Node.js stack to
play music. On an 8-core desktop, scrolling its track list alone can pull 40%+ CPU
across multiple cores. SpotifyGTK targets a single-digit percentage of one core
for the same workload, by going straight to GTK4's GPU-accelerated rendering and
avoiding a browser runtime entirely.

## Core principles

- **No dependency younger than 5 years** for any critical path
- **Every external library is optional**, with a pure-C fallback — nothing is
  load-bearing enough to break the build if it's missing
- **Runtime probing over compile-time feature gates** wherever possible — the
  binary works everywhere, it just silently takes the best available path
- **Two release tracks** (see below) — one frozen and boring, one bleeding-edge

## Two tracks: Stable & Nightly

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

## Architecture

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

Two independent audio paths exist by design:

- **`api.c`** controls playback via the public Web API — this works *today*,
  but requires an existing Spotify Connect device (phone, desktop app, speaker)
  to actually produce sound. This is what's wired up and functional right now.
- **`spotify/` + `audio/`** is the standalone playback engine — SpotifyGTK
  becomes its own Connect device and decodes/outputs audio itself, no other
  client required. This is the **target architecture** and is under active
  development; see Project Status below for exactly what's real vs. scaffolded.

## Project status

Being direct about what's implemented vs. scaffolded, because a stream cipher
or DH handshake that *looks* done but is subtly wrong is worse than an honest gap:

| Module | Status |
|---|---|
| OAuth 2.0 PKCE (`auth.c`) | ✅ Functional |
| Web API wrapper (`api.c`) | ✅ Functional |
| GTK4/libadwaita UI shell | ✅ Functional |
| Image cache (LRU + disk + libjpeg-turbo/stb_image) | ✅ Functional |
| Image cache VA-API hardware decode | 🟡 Probe works, decode path stubbed (see `image_cache.c`) |
| Ogg/Vorbis decoder (`audio/decoder.c`) | ✅ Functional |
| Audio output: PulseAudio, ALSA | ✅ Functional |
| Audio output: PipeWire | 🟡 Implemented, needs validation against a running PipeWire instance |
| Audio output: WASAPI (Windows) | ⬜ Stub only — Windows port hasn't started |
| AP connection + packet framing (`spotify/ap.c`) | 🟡 TCP/SRV resolution real, DH handshake not implemented |
| Shannon cipher (`spotify/shannon.c`) | 🟡 Key schedule/buffering real, round function intentionally stubbed — needs verified reference constants, not reconstructed from memory |
| Mercury protocol (`spotify/mercury.c`) | 🟡 Framing implemented, unverified against live traffic |
| Audio key exchange (`spotify/audio_key.c`) | ✅ Request/response plumbing complete |
| CDN fetch + AES-CTR decrypt (`spotify/cdn.c`) | 🟡 HTTPS Range + decrypt real, IV seed value needs confirming against a captured stream |
| Spotify Connect registration (`spotify/connect.c`) | 🟡 Mercury subscription real, device-state payload is placeholder pending the real protobuf schema |

**In short:** the app runs today, authenticates, and can control playback on an
existing Spotify device via the Web API. The standalone audio engine — the part
that makes SpotifyGTK *not* need another Spotify client running — has its
scaffolding in place but the protocol-critical cryptography is deliberately
left unfinished rather than guessed at.

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

# Pick a track:
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

SpotifyGTK uses **OAuth 2.0 Authorization Code + PKCE** — no client secret is
ever stored on disk.

1. Register an app at [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard)
2. Add redirect URI: `http://localhost:8888/callback`
3. Copy your **Client ID**, then:

```bash
export SPOTIFY_CLIENT_ID="your_client_id_here"
./build/src/spotifygtk
```

Your browser opens for login; the token is captured on `localhost:8888` and
stored via **libsecret** (or `~/.config/spotifygtk/tokens`, `chmod 600`, if
libsecret isn't available).

---

## Project Structure

```
SpotifyGTK/
├── src/
│   ├── main.c               Entry point
│   ├── app.c / app.h        GApplication subclass
│   ├── auth.c / auth.h      OAuth 2.0 PKCE + token storage
│   ├── api.c / api.h        Spotify Web API wrapper (libsoup3)
│   ├── player.c / player.h Playback state + polling
│   ├── image_cache.c/h      LRU + disk cache, 3-tier decode
│   │
│   ├── ui/                  GTK4/libadwaita widgets
│   │   ├── window.c/h
│   │   ├── now_playing.c/h
│   │   └── search_page.c/h
│   │
│   ├── spotify/             Protocol layer — pure C, zero Rust
│   │   ├── shannon.c/h      Stream cipher (AP transport encryption)
│   │   ├── ap.c/h           Access Point connection + framing
│   │   ├── mercury.c/h      Pub/sub metadata protocol
│   │   ├── audio_key.c/h    Per-track decryption key exchange
│   │   ├── cdn.c/h          Chunked encrypted audio fetch
│   │   └── connect.c/h      Spotify Connect device registration
│   │
│   ├── audio/                Playback engine
│   │   ├── decoder.c/h       Ogg/Vorbis → PCM
│   │   ├── output.h          Abstract output interface
│   │   ├── output_pulse.c    PulseAudio (stable default)
│   │   ├── output_alsa.c     ALSA (bare-metal fallback)
│   │   ├── output_pipewire.c PipeWire (nightly)
│   │   └── output_wasapi.c   Windows (stub — port not started)
│   │
│   └── vendor/
│       └── stb_image.h       Vendored from nothings/stb, owned permanently
│
├── data/                     Desktop entry, GSettings schema, icons
├── po/                       i18n (gettext)
├── tests/                    GLib unit tests
├── build-profiles/           stable.ini / nightly.ini
└── .github/workflows/        CI (both profiles) + release pipeline
```

---

## Running Tests

```bash
meson test -C build --print-errorlogs
```

## Contributing

1. Fork, branch off `main`
2. Match existing style (2-space indent, GLib naming conventions)
3. Run `cppcheck src/ -i src/vendor` before submitting
4. If touching `spotify/shannon.c` or `spotify/ap.c`: cite your source for any
   cryptographic constants — see the file header comments for why this matters

### Roadmap

- [ ] Shannon cipher round function (needs verified reference constants)
- [ ] AP Diffie-Hellman handshake
- [ ] VA-API hardware JPEG decode path (probe works, decode pending)
- [ ] Mercury protocol validation against live traffic
- [ ] Album/artist browse pages
- [ ] MPRIS2 D-Bus interface
- [ ] Flatpak packaging
- [ ] Windows port (WASAPI backend, MSYS2/MinGW build)

---

## License

**GNU General Public License v3.0** — see [LICENSE](LICENSE).

SpotifyGTK is an independent open-source project, not affiliated with or
endorsed by Spotify AB. "Spotify" is a trademark of Spotify AB.

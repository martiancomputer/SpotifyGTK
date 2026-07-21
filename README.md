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
│   └── playback/              Audio key + CDN decrypt, streaming auth-relay chain
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

**Currently a development engine plus a basic GTK4 shell, not a finished
client.** The shell reports verified engine capability and deliberately keeps
unwired playback controls disabled; the standalone harness remains the
protocol/playback validation tool. See Project status below for the exact
breakdown.

### Building

```bash
cd apps/spotify-native
meson setup build --native-file build-profiles/stable.ini
# or: meson setup build --native-file build-profiles/nightly.ini
ninja -C build
./build/src/spotify-native-harness
# Basic GTK4 engine shell:
./build/src/spotify-native
```

The GTK shell launches `spotify-native-harness` as its current playback
bridge; both executables are installed together by `ninja -C build install`.

For a headless engine/harness build, configure Meson with
`-Denable_gui=false`.

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
| Shannon cipher (`spotify/shannon.c`) | ✅ Confirmed against real ground-truth test vectors (a compiled-and-run copy of the actual reference crate, not read-and-reasoned-about). Note: the internal round-trip self-test alone had passed for a while despite a real bug (`sbox1`/`sbox2` used XOR where the reference uses OR) — that class of test can't catch a deviation applied symmetrically to both encrypt and decrypt. See `research/auth/` for the full writeup. |
| Ogg/Vorbis decoder (`audio/decoder.c`) | ✅ Confirmed against a live decrypted Spotify CDN range: 44.1 kHz stereo, 86,592 PCM frames from the initial 64 KiB probe. |
| Audio output: PulseAudio, ALSA | ✅ Functional; PulseAudio live-validated with 86,592 decoded Spotify PCM frames. |
| Audio output: PipeWire | 🟡 Implemented, needs validation against a running PipeWire instance |
| Audio output: WASAPI (Windows) | ⬜ Stub only — Windows port hasn't started |
| AP handshake (`spotify/ap.c`, `dh.c`, `handshake_crypto.c`, `protobuf_min.c`) | ✅ Confirmed working against a real Spotify server — DH exchange, RSA server-signature verification, and HMAC-SHA1 key derivation all checked out. |
| AP login (`spotify/ap.c`, `spotify/native_auth.c`) | ✅ Confirmed working against a real Spotify server. Four real bugs found and fixed on the way here, each caught by a live server rejecting the previous fix rather than by any test that existed at the time: wrong client_id/scopes, a wrong protobuf field number plus missing required fields in the login message, a Shannon cipher bug (the actual root cause underneath both) that broke every encrypted packet regardless of content, and — found after login was already succeeding — a wrong field number for `canonical_username` that silently dropped it from every login. |
| Mercury protocol (`spotify/mercury.c`) | 🟡 Framing implemented, unverified against live traffic |
| Audio key exchange (`spotify/audio_key.c`) | ✅ Confirmed against a live AP session; retrieves a usable 16-byte AES key. |
| Streaming auth-relay (`spotify/clienttoken.c`, `spotify/login5.c`, `spotify/spclient.c`) | ✅ Confirmed live: reusable AP credential → client-token → login5 bearer → track metadata → CDN URL resolution. `spclient.c` still uses librespot's hardcoded fallback host rather than full `apresolve`. See `research/playback/`. |
| CDN fetch + AES-CTR decrypt (`spotify/cdn.c`) | ✅ Confirmed live for the initial range and an independent non-block-aligned range; plaintext begins with `OggS` and decodes to PCM. |
| Native playback worker (`native_engine.h`, `player_service.c`) | 🟡 Runs the validated pipeline off the GTK thread, reports CONNECTING/BUFFERING/PLAYING stages, feeds decrypted CDN ranges into a bounded PCM queue with a dedicated output worker, propagates cancellation, and supports pause/resume; queue and seek control are next. |
| Spotify Connect registration (`spotify/connect.c`) | 🟡 Mercury subscription real, device-state payload pending real protobuf schema; ad-insertion events not yet researched |
| Image cache VA-API hardware decode | 🟡 Probe works, decode path stubbed (lives in `spotify-connect`, shared concept) |
| Vulkan compositing | ⬜ Not started |
| GTK4 UI shell (`gui_main.c`, `ui/`) | 🟡 libadwaita shell (`ui/`, built when libadwaita ≥ 1.4 is present; `gui_main.c` remains the fallback): sidebar navigation, persistent playback bar, live connection/buffering/playing stages, pause/resume. **Search and Liked Songs run entirely on the native protocol stack** — confirmed in the running GUI, which signs in via the session and renders 100 real tracks with no Web API call. Home and Library are static explanatory pages until their endpoints are ported (see below). Queue and seek remain to be integrated. `SPOTIFY_START_PAGE=<name>` opens straight onto a page for development. |
| ⚠️ Catalog data path | ✅ **Migrated off the Web API.** The native shell used to mint a token from the keymaster `client_id` (`native_auth.h`) and send it to `api.spotify.com`. Spotify meters the Web API per `client_id`, and that one is Spotify's own internal ID — shared by every librespot user — so every request drew on a globally-contended quota and returned `429 API rate limit exceeded`. Diagnosed by elimination: no token and a garbage token both return `401` from the same host, so a 429 required a *valid* token on a throttled `client_id`, and `Retry-After` moved independently of our own request rate. librespot never calls `api.spotify.com` at all (zero references in its repo). `apps/spotify-native/src/ui/` now contains no reference to the Web API client. |
| Reusable native session (`spotify/session.c`) | ✅ Confirmed live. Holds the AP login → client-token → login5 chain open instead of tearing it down per playback attempt, and serves catalog queries against it. Runs its own worker thread and `GMainContext` — the same isolation `run_live_test()` uses, so protocol callbacks can never dispatch on the GTK thread — while the public API is main-thread-facing and returns results via `GTask`. `load_tracks()` does both round trips (context-resolve, then batched metadata) because neither alone yields a renderable row. Written against the existing public modules rather than by refactoring `main.c`, so the playback harness is untouched. |
| Track display metadata (`spotify/track_meta.c`) | ✅ Confirmed live. Extracts name/album/artist/duration/explicit from the `Track` message `spclient.c` already fetches for playback and previously discarded. Field numbers transcribed from `metadata.proto` and asserted as literals in `tests/test_track_meta.c`. **`duration` is zigzag-encoded `sint32`** — an earlier revision of this file asserted the opposite in a comment, and reading it as a plain varint returned exactly double (Rick Astley's "Never Gonna Give You Up" as 7:07 instead of 3:33). Only live data caught it; the offline round-trip test had happily agreed with the wrong encoder. Now pinned by a regression test using real track lengths. |
| Context resolution (`spotify/spclient.c`) | ✅ Confirmed live. `/context-resolve/v1/{uri}` — the single endpoint behind search, liked songs, and playlist/album/artist listings; returns JSON, unlike the rest of spclient. Search for "never gonna give you up" returned 20 correct tracks; `spotify:user:<id>:collection` returned a 4,773-track library. **`ContextTrack` carries `uri` and nothing else** — no title, artist, or metadata map at all, so this is a URI list and every rendered row requires the batched-metadata call below. URI builders are unit-tested for escaping so a query cannot inject URI structure (`tests/test_context_uri.c`, 9 cases). |
| Batched display metadata (`spotify/spclient.c`) | ✅ Confirmed live. `BatchedEntityRequest.entity_request` is repeated, so a whole page resolves in one round trip. Verified at 50/200/500/1000/2000 URIs; a single request for all 4,773 collection tracks fails, so the ceiling is between 2000 and 4773 — callers should page well below that anyway. Results correlate via `EntityExtensionData.entity_uri` rather than request ordering, so a partial or reordered response still lines up with the right row. |
| Live probe (`main.c`, `SPOTIFY_PROBE_CONTEXT`) | ✅ Env-gated diagnostic that runs the full chain (AP login → client-token → login5 → context-resolve → batched metadata) and prints what the UI would render. `SPOTIFY_PROBE_CONTEXT="some query"` searches, `="collection"` lists liked songs, or pass any `spotify:...` URI; `SPOTIFY_PROBE_LIMIT` caps the batch. Replaces playback rather than preceding it, so a diagnostic failure can't be confused with a pipeline failure. |
| Playlist rootlist | ⬜ Not started — the one remaining catalog gap. `/playlist/v2/user/<user>/rootlist` returns `playlist4_external` protobuf, a much larger schema than `Track`, so the Library page cannot list playlists yet. Playlist *contents* already work: context-resolve handles `spotify:playlist:<id>` like any other URI. |
| Listening history | ⬜ Blocked upstream — no spclient endpoint for recently-played is known (librespot exposes none), so Home's "Recently Played" has nothing to port. |

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
- [x] Confirm AP login succeeds end-to-end with the corrected native_auth token
- [ ] Confirm the streaming auth-relay chain (client-token → login5 → spclient) against real services
- [x] Extract the auth-relay into a reusable session object
- [x] Move the catalog off `api.spotify.com` onto spclient context-resolve + extended-metadata
- [ ] Parse `playlist4_external` so the Library page can list playlists
- [ ] Page beyond the first batch of a large collection
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

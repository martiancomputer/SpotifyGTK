# SpotifyGTK engineering guide

This document is the detailed engineering README for SpotifyGTK. It is
separate from the user-facing [`README.md`](../README.md) on the repository
front page. It records the current architecture, implementation decisions,
performance investigations, debugging methods, and known limitations. The
history below is based on the fixes and live investigations made while
building the native client.

Status in this document is derived from the committed implementation and the
conversation/log investigations that led to it. `to-do.md` is not an authority
for current status: many fixes landed without its corresponding checkbox or
note being updated, and some entries describe symptoms that were later solved
by a different change. When this guide and the TODO list disagree, inspect the
source and commit history first.

The native application is written in C with GTK4/libadwaita. Rust, Glycin,
Electron, and a Rust image runtime are intentionally not dependencies.

## Project shape

```text
apps/spotify-native/              Full player, protocol stack and GTK UI
apps/spotify-native-windows/     Windows/MSYS2/WASAPI build and packaging
apps/spotify-connect/            Small HTTP/JSON Connect control client
research/                         Protocol and live-server notes
docs/                             Cross-cutting engineering documentation
tests/                            Offline protocol, DSP and settings tests
```

The two applications are separate products. `spotify-native` owns encrypted
streaming, decoding, audio output and the GTK shell. `spotify-connect` is a
smaller control client with different dependencies and must not be assumed to
share the native application's caches or event-loop ownership.

New native functionality belongs in the existing source modules. In
particular, do not add new `.c` or `.h` files without an explicit structural
reason; the current codebase already has the appropriate extension points.

## Runtime architecture

```text
GTK/libadwaita shell (src/ui/)
        │ page models, navigation, settings, scrolling
        ▼
SpotifyNativeSession (src/spotify/session.c)
        │ AP login, credentials, catalog and Connect state
        ├── Mercury requests/responses/subscriptions
        ├── spclient context and extended metadata
        ├── audio-key and CDN URL resolution
        └── Connect dealer/device state
        ▼
Playback worker (player_service + native_engine)
        │ CDN ranges → decrypt → Ogg/Vorbis → PCM sink
        ▼
PulseAudio / ALSA / PipeWire / WASAPI
```

GTK owns widgets and list models. Network, decode and audio work must not block
the GTK thread. Worker state machines use their own thread-default GLib
contexts. A callback or retry must be attached to the context that owns the
state it mutates; plain `g_timeout_add()` is dangerous for a private worker
because it uses the global default context.

### Source map

| Area | Existing code | Purpose |
|---|---|---|
| Authentication | `src/spotify/ap.c`, `native_auth.c`, `clienttoken.c`, `login5.c` | AP/DH login and bearer credentials |
| Mercury | `src/spotify/mercury.c`, `mercury.h` | Framing, requests, replies and subscriptions |
| Catalog | `src/spotify/spclient.c` | Context resolve and metadata |
| Collections | `src/spotify/collection.c` | Liked albums/tracks and change events |
| Playback | `src/player_service.c`, `src/native_engine.*`, `src/audio/` | Engine, decoder, sink and outputs |
| Artwork | `src/ui/cover_loader.c` | CDN, disk/memory cache and decode queue |
| Track lists | `src/ui/track_list.c`, `track_row.c` | Virtualized rows and overscan |
| Album cards | `src/ui/album_grid.c` | Album/playlist grids and card ownership |
| Scrolling | `src/ui/smooth_scroll.c` | Discrete wheel animation |
| Pages/shell | `src/ui/*_page.c`, `window.c` | Page-specific models and shared UI |
| Settings | `src/ui/settings.c`, `settings_page.c` | Persisted preferences and controls |

## Protocol and data flow

### Authentication and session lifecycle

First run opens a browser for authorization. A stored token is only a
credential, not proof of a usable connection: the login gate remains until the
session reports `READY` and returns on `FAILED`. The live-validated sequence is
AP handshake, DH exchange, Shannon-encrypted login, client-token, login5
bearer, spclient catalog access, Mercury subscriptions and Connect setup.

The Shannon implementation is checked against reference vectors. A symmetric
encrypt/decrypt round trip is not enough: an earlier s-box XOR-versus-OR error
passed a symmetric test while failing against the real server.

### Mercury

Mercury's receive half was added after the initial request writer. Responses,
subscriptions and pushed collection events now work. The wire layout is:

```text
[u16 sequence length][sequence][u8 flags][u16 part count]
[length-prefixed parts]
```

Part zero is a protobuf header containing URI, content type, method, status and
user fields. The client enforces the measured 16,384-byte request ceiling and
returns a local failure for oversized requests because Spotify does not
reassemble fragmented requests.

A key timeout bug was mutable request metadata: a collection-v2 content type
leaked into later playlist/rootlist requests. Dispatch now snapshots content
type and logs method, URI and type in verbose mode.

### Catalog

The current native UI uses spclient context-resolve and extended metadata, not
the Web API. Album shelves are grouped from resolved tracks because there is
no separate album-listing endpoint in the native path. Track album/artist gids
are converted to real URIs and open the shared context page.

Search's native endpoint is a playback context, not a conventional ranked text
search endpoint. This explains why a playable context can begin with an item
that is not the strongest textual match; local filtering/ranking cannot change
what Spotify's context endpoint returns.

## UI and page models

`window.c` owns navigation history, the current session, shared liked/followed
URI sets, playback context/queue, the playlist grid and the global loading
indicator. Pages own their models and expose narrow load/session APIs.

GTK's `GtkStack` supplies pages; the shell adds a navigation sidebar, playback
bar and optional Now Playing panel. Back/Forward use a browser-like history
stack. Revisiting a page is not supposed to refetch every object.

### Reconnect behavior

The same `SpotifyNativeSession` object can emit READY again after an AP
reconnect. Page `set_session()` methods now recognize same-object READY events
and preserve resolved models, names, URI sets and in-flight work. Previously,
every READY cleared Library, Liked Songs, Home, Artist and Context, creating
app-wide loading stutter and making caches appear broken. A genuinely new
session still increments a generation, cancels old work and discards old data.

Every asynchronous callback that can outlive a page should use a generation,
weak reference, cancellable or equivalent ownership check.

## Artwork, memory and caching

`ui/cover_loader.c` owns the artwork pipeline:

1. Look up the `(cover id, target size)` texture in the memory LRU.
2. Look up the compressed image in the disk cache.
3. Fetch the CDN image if necessary.
4. Decode on a worker using in-process C `libjpeg`/`libpng`.
5. Decode near the requested size and create a `GdkMemoryTexture`.
6. Complete all waiters on the GTK thread.

Glycin/image-rs was deliberately removed. Runtime measurements showed a CPU
spike with Glycin that disappeared when it was eliminated. Keeping decoding in
the existing C loader makes concurrency, allocation and target-size decisions
visible and controllable.

The compressed disk cache survives restarts and is bounded at roughly 1 GB.
Decoded textures use a bounded roughly 24 MiB LRU. The cache key includes size:
a 96-pixel row texture must not satisfy a large Now Playing request. Aggressive
media mode raises bounded worker concurrency and releases decoded textures when
widgets no longer need them; it is not unlimited parallelism. Settings can
disable caching, clear cached names/media and toggle aggressive loading.

Cache statistics distinguish requested, joined, fetched, disk-hit, deferred,
dropped and failed work. A large “decode” duration can include queue/dispatch
age, so inspect active/queued counts before blaming the codec.

### The Library 100 MiB investigation

The album page showed a roughly 100 MiB RSS spike with only 10–14 visible
covers. Logs showed approximately 257 bound cards, 24 mapped/retained cards, a
peak queue near 241 and only about 12–15 MiB of steady mapped pixels. The
transient was queued decode/fetch buffers and widget references, not a 100 MiB
visible-cover cache.

The cause was a GTK recycling race: `factory_bind()` ran while a recycled card
was still mapped with its previous allocation. Immediate viewport testing
therefore classified nearly the whole model as visible. Replacing cancellables
also created a cancel/retry feedback loop. The fix defers selection until a
post-layout settle pass, keeps one request owner per card, and releases cards
outside the settled window.

## Viewport virtualization and overscan

Binding, mapping and actual viewport position are different facts. GTK may bind
or map more widgets than the user can see.

### Track lists

`track_list.c` uses velocity-aware overscan: eight base rows, up to 32 rows in
the direction of travel, a small trailing window, one coalesced settle update,
row-owned cancellables and cover release after the gesture. Prefetch uses the
same small cover id and target size a row will request; full-size art under a
different key would cost more and be unusable by the row.

### Album and playlist grids

The grid uses the visible cards plus one row/column of look-ahead:

```text
bind/recycle → labels and placeholder only → schedule settle
GTK allocation → measure bounds → resolve/load inside window
                             → release outside window
```

`factory_bind()` and `card_apply_item()` never begin expensive work from stale
geometry. `card_retry_cover()` refuses to replace a live request. The grid no
longer toggles the global cover-loader deferral flag, so one page cannot change
another page's loading behavior.

## Smooth scrolling

`smooth_scroll.c` intercepts discrete mouse-wheel events in capture phase and
animates a `GtkAdjustment` toward an accumulated target. Continuous touchpad
events remain with GTK's native kinetic scrolling.

The animation uses elapsed-time easing, absorbs small list anchor corrections,
treats large adjustment changes as scrollbar/keyboard/programmatic ownership,
re-clamps when content bounds change, and stops when pixel quantization would
otherwise leave a permanent frame callback.

Settings → Interface → Scroll smoothness persists `scroll-smoothness` from 0
to 100. The midpoint is the existing tuned behavior; it controls wheel travel
and easing together because users perceive them as one gravity/response
character:

| Value | Travel/notch | Easing | Character |
|---:|---:|---:|---|
| 0 | 88 px | 0.44 | Responsive, short and quick |
| 50 | 118 px | 0.30 | Existing default |
| 100 | 148 px | 0.16 | Glide, farther and softer |

The value is read live by the scroll controller and does not affect touchpad
motion.

## Search

Search originally waited on serialized result work. The current native loader
keeps the 300-result behavior, fetches Pathfinder pages in parallel, merges and
ranks them, then updates the model once. Updating once is essential: appending
each page as it arrives refreshes every object in the view and makes pagination
feel slower.

Activating a search result creates a song-radio context. Recommendations are
deduplicated and expanded toward a bounded 200-track queue. The endpoint's
playback-context semantics remain a known relevance limitation.

## Library and playlists

Liked Songs is paged through collection-v2. Date-added keeps the collection's
newest-first order because `added_at` is present in that response; Length and
A–Z are local sorts. Shared liked URI state keeps rows, menus and the playback
bar consistent. Library resolves saved album URIs in batches and separates
albums, EPs, singles and followed artists. Artist names use a small persistent
metadata index.

The playlist rootlist interleaves folder markers and playlist URIs. Markers are
filtered first. A playlist card initially knows its URI; its head supplies the
name and a one-track context supplies a cover. Resolution is driven by the
settled viewport, preventing cold startup from issuing two requests for every
playlist. Creating, deleting and renaming playlists work through existing
operations. Playlist artwork upload is not implemented because it is a separate
image-upload endpoint.

## Playback and Connect

The playback path is:

```text
metadata → audio key → CDN ranges → AES-CTR → Ogg/Vorbis → PCM sink → output
```

The audio device and sink outlive a track. Per-track frame queues drain in
order, allowing the next track to decode before the current track ends and
removing device close/reopen gaps. Position is based on frames written to the
device. Seek walks Ogg pages, replays cached headers and fetches a landing
window; EOF responses receive special handling.

Volume is software PCM scaling for consistent backend behavior. The 15-band EQ
uses RBJ peaking biquads in the audio worker. The optional polyphase
windowed-sinc resampler is a byte-exact passthrough at equal rates.

The UI adds Previous/Next, queueing, ordinary shuffle, repeat-one,
repeat-all, radio expansion and Connect-aware Smart Shuffle over the engine.
Connect registration uses the dealer WebSocket, reports track/state/position,
accepts remote transport commands and yields to a higher-ranked device.

## Settings and controls

Preferences are persisted in:

```text
~/.config/spotify-native/settings.ini
```

The existing settings singleton stores theme, media mode, renderer, EQ gains,
caching, aggressive media, shuffle/repeat and `scroll-smoothness`. Setters save
before emitting `changed`, so listeners see the new value.

The UI exposes four themes, artwork policy, cache controls, aggressive media,
scroll smoothness, GSK renderer selection, sample rate, EQ and account profile.
Renderer changes require restart because GSK chooses one renderer per process;
an explicit `GSK_RENDERER` environment value takes precedence.

### Settings reference

| Section / control | Stored key | Values | What it changes | When it applies |
|---|---|---|---|---|
| Interface → Theme | `theme` | Dark, White, Milk, Dark+ | Selects the application palette. Accent green is reserved for state such as liked, followed, pinned and selected items. | Immediately; CSS is reloaded. |
| Interface → Previews | `media-mode` | Media, Now playing only, None | Controls which artwork surfaces may request covers. “Now playing only” prevents list/grid artwork work; “None” prevents artwork requests altogether. | Immediately for new requests; existing images are released by the loader. |
| Interface → Scroll smoothness | `scroll-smoothness` | 0–100 | Tunes mouse-wheel travel and easing together. Low values are shorter and more responsive; high values carry farther with softer gravity. | Immediately for wheel events and animation frames. Touchpad kinetics are unchanged. |
| Search Settings → Aggressive Filtering | `aggressive-filtering` | Off/On | Changes local search matching/ranking so exact artist/title matches are promoted while weaker matches remain available. It does not change Spotify's server-side context result. | Immediately on the next filter/search update. |
| Audio → Sample rate | `sample-rate` | Default, 44.1 kHz, 48 kHz, 96 kHz | Chooses the device rate. Default follows the stream; another rate activates the native polyphase windowed-sinc resampler. | Persisted immediately; used when the next output device/track is opened. |
| Audio → Sample format | — | Native, 24-bit | Present as an explicit unavailable option. The current PCM path is 16-bit end to end, so this control is intentionally insensitive. | Not implemented. |
| Audio → Resampler | — | Native | Documents the active native resampler. There is no alternate implementation to select. | Not applicable. |
| Audio → Equalizer enabled | `eq-enabled` | Off/On | Enables the 15-band RBJ peaking-biquad cascade in the audio worker. Disabled or flat is a byte-exact no-op. | Immediately for live audio. |
| Audio → Equalizer bands | `eq-gains` | 15 bands, −12 to +12 dB | Stores the gain curve from 25 Hz through 16 kHz. The UI uses a draggable response curve rather than fifteen independent slider strips. | Immediately and persisted per band. |
| Performance → Renderer | `renderer` | Automatic, Vulkan, OpenGL, Cairo | Selects the GSK compositor. Cairo is software rendering; Vulkan/OpenGL are explicit alternatives for diagnosis or hardware-specific behavior. | Saved immediately, applied after restart. `GSK_RENDERER` overrides it. |
| Performance → Caching | `caching-enabled` | Off/On | Enables the compressed artwork/name disk cache and decoded texture cache. Disabling it also prevents cache reads and trims decoded textures. | Immediately. |
| Performance → Aggressive media loading | `aggressive-media` | Off/On | Raises bounded artwork worker concurrency and releases decoded textures as soon as widgets stop holding them. It does not remove queue or memory limits. Requires caching. | Immediately. |
| Performance → Clear cache | — | Action | Removes persisted names/media and trims in-process decoded artwork. It is an action, not a preference. | Immediately; later requests repopulate it. |
| Playback state → Shuffle | `shuffle` | Off/On | Persists the ordinary play-order preference. It changes the UI play context, not the server's Smart Shuffle capability. | Immediately for the active queue and future contexts. |
| Playback state → Repeat | `repeat` | Off, one, all | Controls end-of-context behavior. Repeat-one replays the current entry; repeat-all restarts the context. | Immediately. |

Some settings are deliberately represented in the interface even when there is
no implementation behind them. The insensitive Sample format and Resampler
dropdowns document the audio design without pretending that a nonexistent
24-bit or alternate converter can be selected. Account identity fields are
runtime information rather than preferences: the display name, canonical id,
product tier and avatar come from the signed-in session and are not written to
the settings file.

The settings file is a GLib key file at
`~/.config/spotify-native/settings.ini`. Setters persist before emitting the
singleton's `changed` signal, so window-level listeners can safely apply the
new theme, EQ or sample-rate state. The one exception to “live” rendering is
the GSK renderer, whose backend is selected once during process startup.

## Diagnostics

Configure verbose logging when investigating a live issue:

```bash
meson setup build -Dverbose_logging=true
ninja -C build
SPOTIFY_COVER_STATS=1 SPOTIFY_SCROLL_STATS=1 \
  G_MESSAGES_DEBUG=all ./build/src/spotify-native
```

Useful switches are:

```text
SPOTIFY_COVER_STATS   cover queue/cache statistics at settle boundaries
SPOTIFY_SCROLL_STATS  wheel input, frame gaps and ownership transitions
SPOTIFY_NAV_PROBE     development navigation probe
SPOTIFY_DEV_START_PAGE  page to open for a probe
```

Always record page/navigation time, RSS before/during/after scrolling, cover
asked/fetched/joined/deferred/dropped counts, active/queued jobs, mapped and
texture-holding cards, session generations, and scroll frame gaps. A lower
steady RSS after a peak can be allocator high-water rather than retained
textures; a growing mapped/held count after settle is a real ownership bug.

## Build and test

```bash
cd apps/spotify-native
meson setup build --native-file build-profiles/stable.ini
ninja -C build
meson test -C build --print-errorlogs
./build/src/spotify-native
```

The root build tree is also used for CI and probes:

```bash
ninja -C build
meson test -C build --print-errorlogs
```

The offline suite covers Shannon, protobuf/wire helpers, settings, AP parsing,
DSP, resampling, collection writes, login constants and streaming-auth
encoding. Live protocol behavior requires a real session; use disposable
playlist operations when testing writes. `-Dallow_old_gtk=true` supports older
GTK packages. The nightly profile selects PipeWire. Windows uses the MSYS2
UCRT64/WASAPI instructions in its application README.

## Contribution rules

1. Keep GTK work on GTK's thread; keep network/decode/audio work off it.
2. Attach GLib sources to the owning thread-default context.
3. Never equate `bind` or `mapped` with viewport visibility.
4. Give every asynchronous request an owner, generation, cancellable or weak
   reference and reject stale callbacks.
5. Coalesce model updates and scroll sources.
6. Bound both concurrency and memory; use the smallest useful artwork size.
7. Preserve models across same-session reconnects.
8. Extend existing C modules; do not add Rust/Glycin or new C/header files.
9. Add offline regression tests for pure logic and verbose diagnostics for live
   behavior.
10. Label live-verified facts, measured inference and unimplemented work
    separately in research notes.

## Current status and limitations

Live-validated areas include authentication, Shannon/DH, Mercury responses and
subscriptions, spclient catalog access, native search, Liked Songs, Library,
playlists, album/artist contexts, artwork caching, gapless playback, seeking,
EQ, resampling, Spotify Connect, Smart Shuffle integration, themes, renderer
selection and Windows WASAPI playback.

Open or deliberately limited areas include:

- native search relevance differs from Spotify's consumer ranked search;
- playlist artwork upload;
- broader PipeWire validation;
- VA-API hardware JPEG decode in the Connect application;
- ad-insertion/feature-state research;
- MPRIS2 and Flatpak packaging;
- fully removing the old Web API path from the non-libadwaita fallback; and
- rare source-removal/scroll edge cases that need reproduction before another
  speculative fix.

## Chronology of important fixes

### Search latency

An on-demand 50-result experiment was faster per request but refreshed the
whole search page when the next page arrived. It was replaced by parallel
Pathfinder loading with one 300-result model update.

### Artwork CPU and memory

Glycin/image-rs caused a measured CPU spike and was removed. The first custom
viewport implementation still decoded too many cards; repeated cancellation
then caused a retry loop. Target-sized C decode, bounded queues, ownership
guards and post-layout selection resolved the underlying resource problem.

### Mouse-wheel behavior

Touchpad scrolling was already smooth because GTK received continuous deltas;
mouse wheels needed a capture-phase target animation. Anchor corrections and
elapsed-time easing fixed oscillation and frame-rate-dependent stutter. The
current slider exposes the tuned response without touching touchpad kinetics.

### Library/Playlist timeout and cache loss

Mercury content-type contamination caused request timeouts. Repeated READY
events then reset page models on reconnect, making names/media appear uncached.
Dispatch snapshots and same-session guards address the two independent causes.

### Album-grid 100 MiB spike

Logs separated visible texture memory from queued decode buffers and stale
widget references. Deferring work until layout, refusing duplicate card jobs,
and removing global cover deferral fixed the fan-out and cross-page race.

## Related documents

- [Root README](../README.md) — user-facing requirements and quick start.
- [Internals research](../research/internals.md) — subsystem status and live
  validation details.
- [Connect research](../research/connect.md) — Connect protocol notes.
- [Windows README](../apps/spotify-native-windows/README.md) — Windows build.
- [Third-party licenses](../THIRD_PARTY_LICENSES) — attribution obligations.

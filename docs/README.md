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
| Desktop media control | `src/ui/window.c` | MPRIS2 service, metadata and transport bridge |
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

Search also requests up to 20 playlist results through the native bearer /
client-token Pathfinder `searchDesktop` query. This is a separate, cancellable
request started after the main tracks finish, so playlist lookup failures do
not delay or erase the 300-result song search. Albums remain first, songs next,
and the optional playlist shelf last, even with Aggressive Filtering disabled.
With that setting enabled, playlist names or owners must match the normalized
query; adding playlists does not silently disable filtering. Results always
identify their type: Track, Album or Playlist. Playlist cards open playlist
details and use playlist actions rather than being mistaken for albums.

Compact mode places albums and playlists in the same vertical, recycled list
as songs, with the same 40px thumbnails and explicit type labels. Each distinct
album follows its first matching song; unpaired albums precede playlists,
which follow the primary results. The horizontal shelves are hidden entirely.
Switching it off restores the larger artwork shelves around the song list.
The Search title and entry are ordinary page content, not a pinned overlay:
they scroll away with results in either mode. Changing the layout retains song
item objects and does not rerun the search. Late playlist answers splice only
the changed tail of compact results instead of rebinding primary rows. Context
rows open details and reuse card menus (including Share Album/Share Playlist),
but are excluded from the audio playback snapshot and Connect track queue.
Artwork release preserves the card's current size, and pooled card shells
reconcile their layout on bind, including after search → album → search.
The entire results page uses one vertical viewport, which both expanded
shelves and the mixed list observe before loading artwork.
A typed query immediately invalidates older responses, even
during the debounce interval. Unsupported mosaic/custom playlist images retain
a placeholder; recognized Spotify image IDs use the same custom cover loader
as all other artwork. The optional persisted query can change server-side;
failure is logged and primary results remain usable.

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

### Album and playlist detail layouts

Settings → Interface → Compact mode is saved as `compact-mode`. It defaults to
On to preserve the established small title/year/action header and thumbnail
rows for existing users. Switching it Off presents a larger 256px square cover at the
left, a large wrapping title at the right, and a metadata row beginning with
the existing Save/Saved or playlist action. The row includes album year when
known, artist (or “Multiple artists” for collaborations/compilations), track
count and summed duration. A neutral theme-colored separator precedes numbered
songs. No global search bar, navigation redesign or Now Playing/Queue/Lyrics
layout change is involved.

Both detail modes use one page-level scroller: the header, metadata and divider
scroll away with the songs, like the artist page. There is no fixed hero above
an independently scrolling track pane. Opening a different context returns to
the top; revisiting the same context retains its current scroll position.

Expanded songs do not request a redundant album thumbnail for every row. The
one header image uses Now Playing's square `COVER` fit and 12px `art-large`
corners. Known card artwork is reused when opening a detail page; otherwise
the first available song cover is a fallback. Unmapping the page, switching
to Compact mode or disabling page artwork cancels/releases that header image.
Changing modes preserves the loaded list model rather than refetching tracks.
Scrolling past the expanded header's retention margin also releases its cover;
returning to the top reloads that one image through the custom loader.
Long titles wrap/ellipsize without imposing their full text width on the window.
In narrow panes the artwork and text stack into separate rows rather than
compressing the title beside a large cover. The square crop stays unchanged.

Detail pages resolve up to the native context cap of 10,000 tracks instead of
the former 200-track preview, then fetch ordered display metadata in existing
batches. This enables useful playlist counts and durations without loading all
covers. At the cap the count says “tracks loaded”; omitted/unavailable metadata
can still make totals partial, and playlists do not borrow the first song's
release year as a fictitious playlist creation date.

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

Connect state includes duration, correctly encoded double playback speed,
repeat/shuffle options, previous/next tracks, occurrence IDs and a queue
revision. Its next-track snapshot contains the explicit user queue followed by
the remaining playback order, not just the 40-song UI preview. Controllers can
extrapolate the timestamped position between action reports; the 250ms position
poll does not initiate HTTPS requests or copy the queue. HTTP state updates reuse
one session and are serialized: updates arriving during a PUT coalesce into the
latest pending state. Older timestamped cluster snapshots cannot override a
newer dealer/HTTP snapshot's ownership.
Connect checks bearer expiry on its own keepalive/action path instead of
waiting for a catalog request to trigger refresh. A 401 forces a native login5
refresh and republishes the latest state. Immediate auth recovery is limited
to once per 30 seconds so a persistent rejection cannot create a refresh/PUT
loop. This addresses repeated 401s observed in a long-running idle client;
the corrected path still needs a live expiry/recovery test.

Structured remote commands support pause/resume, explicit seek (including
zero), next/previous, selecting another playback context/song, partial repeat
and shuffle changes, adding to the queue and replacing/removing queue entries.
Queue edits are processed in arrival order, reuse known display metadata and
resolve only unfamiliar URIs. An incomplete resolution or stale queue revision
does not erase the working queue. Replacing a queue does not restart or seek the
currently sounding song. Duplicate tracks remain distinct occurrences, and
context-supplied UIDs are retained when resolving a controller's selection.

A same-song controller state update is not an implicit seek: it retains the
audible position instead of adopting a stale zero. Returning ownership from a
different device is distinct and can adopt that device's supplied position.
Loaded-track detection examines audible/buffered and pending engine tracks,
not merely whether a decoder task still exists or what its last progress state
was. A decoder can finish while its buffered audio is still playing. Playback
picks invalidate old transfers still resolving metadata, and a play-with-seek
command holds its position for a pending replacement rather than seeking the
canceled outgoing decoder. Yielding ownership pauses without immediately
reporting the old song as active and reclaiming the device.

These paths have offline wire/JSON regression coverage, but real controller
interoperability still needs a phone/desktop session test; Spotify's private
Connect protocol is not a guaranteed public API. Field and command shapes were
checked against the [extracted player protocol](https://github.com/librespot-org/librespot/blob/dev/protocol/proto/player.proto)
and [dealer command definitions](https://github.com/librespot-org/librespot/blob/dev/core/src/dealer/protocol/request.rs).
They are protocol references only, not new Rust dependencies.

### Sharing a song

Right-click a song row on any track-list page and choose **Copy Song Link**.
The same action is available by right-clicking the current song's title/artist
area in the playback bar, even when that song's row is no longer visible.
The action copies a public `https://open.spotify.com/track/<id>` URL to the
system clipboard for pasting into a message or browser. It converts the native
`spotify:track:<id>` URI locally; it does not call the Web API, create a
playlist, or upload any account data. Local songs and malformed/non-track URIs
have no public track URL, so their row action is disabled rather than copying
a misleading link. Menu state owns its own URL copy so a recycled row or a
track change cannot make an open Share menu point at a different song.

Album and playlist cards also expose sharing in their right-click menus:
**Share Album** and **Share Playlist**, respectively. Each copies the matching
`https://open.spotify.com/album/<id>` or
`https://open.spotify.com/playlist/<id>` URL to the clipboard. These use the
same local URI validation as songs; invalid or non-shareable card URIs leave
the action disabled. The open menu keeps a URL snapshot because a virtualized
card can be rebound while the menu is visible.

### Lyrics

The right Now Playing panel has Queue and Lyrics views. Lyrics are keyed to
the *audible* track, not to a track merely decoding ahead for gapless playback.
Lookup is lazy: it begins only when the Lyrics view is opened, and only for
the current track, so leaving Queue selected adds no file or network work.
The first source is a local UTF-8 `.lrc` sidecar named for the track's 22-character
base62 Spotify ID in the user's GLib data directory:

```text
~/.local/share/spotifygtk/lyrics/<track-id>.lrc   (usual Linux path)
```

For example, a sidecar can contain `[00:17.12] First line` and
`[00:21.50] Second line`. Plain text without timestamps is displayed as a
read-only text view. Synchronized lines are parsed once, sorted by timestamp,
and selected by binary search against the player's existing 250 ms audible
position reports. The panel reuses three labels for previous/current/next;
it changes them only when the active line changes, so lyric playback does not
create a per-frame GTK layout or a row widget for every line. Seek, pause and
natural track handover therefore follow the same position/identity signals as
the playback bar.

The Queue/Lyrics selector sits close to the track details. The Font size
dropdown in Settings → Lyrics changes the highlighted and surrounding timed
lines and plain-text lyrics without refetching or reparsing the song.
The initial 19 px highlighted-line size preserves the original appearance;
20, 22, 24, 26 and 28 px are the larger choices. This is saved in
`lyrics-font-size` and applies immediately. Timed lines render inside a
viewport that does not propagate lyric-derived width or height requests:
long lines wrap within the current Now Playing pane and use the remaining
height rather than resizing its artwork, track details or selector. The timed
viewport scrolls internally only if the window is too short for all three
lines, and follows the current line after a lyric change.

Settings → Lyrics → Online lyrics optionally asks LRCLIB for the current song
when no local sidecar exists. It is **off by default** because the request
sends title, primary artist, album and duration to a third-party service.
Requests identify SpotifyGTK with a User-Agent, are asynchronous, and cancel
on track changes. Only one current-track request is allowed; results are
held in a bounded 16-song in-memory cache, with short-lived missing-result
entries. `429` responses honor `Retry-After` by suspending further requests.
Responses larger than 256 KiB are rejected. No Rust or external lyric-rendering
library is bundled into the app. LRCLIB availability and lyric matching are
independent of Spotify's playback/catalog services; not every recording has
matching lyrics.

## Settings and controls

## Release packaging

Version tags (`v*`) run the release workflow and publish three artifacts:

- `.deb`, linked to the host GTK stack and guarded by GTK 4.22/libadwaita 1.9
  runtime dependencies;
- `.AppImage`, built in a Fedora Rawhide container after checking the same
  toolkit floor, then bundling GTK, GIO/GdkPixbuf modules, icons, and a pinned
  Inter font;
- `.msix`, built from the MSYS2 UCRT64 bundle after checking its GTK versions.

The publish job stages the generated files under `releases/<tag>/` for a
uniform release layout and uploads those same files as GitHub Release assets.
The binaries are intentionally not committed back to `main`; each tag remains
reproducible without turning the source repository into a binary archive.

The Windows bundle also carries the Adwaita/hicolor icon themes, GIO modules,
schemas, CA database, and Inter font. The executable sets bundle-relative
loader and fontconfig paths before GTK initializes, so Explorer/MSIX launches
do not inherit the builder's environment. The font is distributed under the
SIL Open Font License; see `THIRD_PARTY_LICENSES`.

Preferences are persisted in:

```text
~/.config/spotify-native/settings.ini
```

The existing settings singleton stores theme, media mode, renderer, EQ gains,
caching, online lyrics, aggressive filtering, shuffle/repeat and
`scroll-smoothness`. Setters save
before emitting `changed`, so listeners see the new value.

The UI exposes four themes, artwork policy, cache controls, aggressive media,
scroll smoothness, optional lyric lookup, GSK renderer selection, sample rate,
EQ and account profile.
Renderer changes require restart because GSK chooses one renderer per process;
an explicit `GSK_RENDERER` environment value takes precedence.

### Settings reference

| Section / control | Stored key | Values | What it changes | When it applies |
|---|---|---|---|---|
| Interface → Theme | `theme` | Dark, White, Milk, Dark+ | Selects the application palette. Accent green is reserved for state such as liked, followed, pinned and selected items. | Immediately; CSS is reloaded. |
| Interface → Compact mode | `compact-mode` | Off/On (default On) | On retains small detail headers and thumbnail rows, and mixes albums/playlists with songs in one vertical search list. Off shows artwork-led detail headers and numbered rows, plus larger search shelves. Result type labels remain visible in either mode. | Immediately; song items are retained and no new search is sent. |
| Interface → Previews | `media-mode` | Media, Now playing only, None | Controls which artwork surfaces may request covers. “Now playing only” prevents list/grid artwork work; “None” prevents artwork requests altogether. | Immediately for new requests; existing images are released by the loader. |
| Interface → Scroll smoothness | `scroll-smoothness` | 0–100 | Tunes mouse-wheel travel and easing together. Low values are shorter and more responsive; high values carry farther with softer gravity. Slider persistence is debounced for 120 ms so dragging does not synchronously rewrite the settings file on GTK's UI thread. | After the slider pauses; touchpad kinetics are unchanged. |
| Lyrics → Online lyrics | `online-lyrics` | Off/On (default Off) | Allows an asynchronous LRCLIB lookup for the current audible song after checking the local LRC sidecar. Sends its title, primary artist, album and duration to LRCLIB; no scan of a playlist or library occurs. | Immediately; an active track is rechecked when toggled. |
| Lyrics → Font size | `lyrics-font-size` | 19, 20, 22, 24, 26, 28 px (default 19) | Sizes the current timed lyric line, scales neighboring lines, and sizes plain-text lyrics. It does not change lyrics loading. | Immediately and retained across launches. |
| Search Settings → Aggressive Filtering | `aggressive-filtering` | Off/On | Changes local search matching/ranking so exact artist/title matches are promoted while weaker matches remain available. It does not change Spotify's server-side context result. | Immediately on the next filter/search update. |
| Audio → Sample rate | `sample-rate` | Default, 44.1 kHz, 48 kHz, 96 kHz | Chooses the device rate. Default follows the stream; another rate activates the native polyphase windowed-sinc resampler. | Persisted immediately; used when the next output device/track is opened. |
| Audio → Sample format | — | Native, 24-bit | Present as an explicit unavailable option. The current PCM path is 16-bit end to end, so this control is intentionally insensitive. | Not implemented. |
| Audio → Resampler | — | Native | Documents the active native resampler. There is no alternate implementation to select. | Not applicable. |
| Audio → Equalizer enabled | `eq-enabled` | Off/On | Enables the 15-band RBJ peaking-biquad cascade in the audio worker. Disabled or flat is a byte-exact no-op. | Immediately for live audio. |
| Audio → Equalizer bands | `eq-gains` | 15 bands, −12 to +12 dB | Stores the gain curve from 25 Hz through 16 kHz. The UI uses a draggable response curve rather than fifteen independent slider strips. | Immediately and persisted per band. |
| Performance → Renderer | `renderer` | Automatic, Vulkan, OpenGL, Cairo | Selects the GSK compositor. Cairo is software rendering; Vulkan/OpenGL are explicit alternatives for diagnosis or hardware-specific behavior. | Saved immediately, applied after restart. `GSK_RENDERER` overrides it. |
| Performance → Caching | `caching-enabled` | Off/On | Enables the compressed artwork/name disk cache and decoded texture cache. Disabling it also prevents cache reads and trims decoded textures. | Immediately. |
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
UCRT64/WASAPI instructions in its application README. The native-auth test has
an opt-in live HTTPS check (`SPOTIFYGTK_TLS_PROBE=1`) for validating a portable
Windows bundle without making the default suite depend on the network.

The Connect tests also cover stale timestamps, same-song position preservation,
explicit seeks to zero, queue encoding/revisions/duplicate occurrence IDs,
structured play and partial option updates, and malformed queue payloads. A
GUI presentation test reuses the existing settings test source and is built
when GTK/libadwaita are available. It skips without a display. On headless Linux:

```bash
GSK_RENDERER=cairo xvfb-run -a meson test -C build --print-errorlogs
```

The GUI test verifies model retention with 300 songs, both compact/expanded
search layouts, a narrow window with a long album title, square cover fit, and
idle frame-clock settling on album and empty artist pages. Setting
`SPOTIFYGTK_UI_CAPTURE_DIRECTORY` to an existing temporary directory enables
PNG captures of these mocked/offline widgets; it does not use real credentials
or alter the running application's settings.
Offline fixtures also exercise the real detail completion path (compilation
artists, full loaded counts/duration, and no invented playlist year) and
playlist matching by name/owner with Aggressive Filtering on and off.
Search regression assertions also check header movement with vertical scroll,
hidden compact shelves, mixed result ordering/activation, song-only playback
snapshots, primary-item retention during playlist changes, and the artwork
release/remap lifecycle of search → album → search. A synthetic paintable
exercises compact card release so oversized placeholders cannot go unnoticed.
Detail regression assertions verify compact/expanded header scrolling, release
of offscreen hero artwork, and top-reset versus same-context scroll retention
for album/playlist navigation.
Address and undefined-behavior sanitizer runs cover the settings, Connect helpers and
UI presentation paths. Leak checking in the standalone settings test exposed
missing finalization of its path/pins/unavailable table, now released when a
settings instance is destroyed. This small shutdown leak is not evidence for
the much larger intermittent scrolling memory/stutter symptom. The GUI sanitizer
run disables leak detection for GTK's process-global caches; it is not a
long-duration whole-application leak test.

Two non-scroll-controller sources of unnecessary work were removed: the
context header's unbounded duration-alignment tick, and offscreen grid cards
rearming settle polling from the settle pass itself. Artist header alignment
also stops after a bounded allocation grace period when there is no scrollbar.
Verbose builds add passive `frames:` samples beside the existing 5-second
memory samples while the frame clock is active (FPS, late-frame count, worst
gap and refresh interval). They inspect existing frame history rather than
requesting redraws. Idle samples alone cannot establish the cause of an
intermittent active-scrolling stutter; correlate these samples with an actual
reproduction and artwork/Connect activity.

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

### MPRIS2 desktop integration

The native window owns `org.mpris.MediaPlayer2.spotifygtk` at
`/org/mpris/MediaPlayer2`. Keeping the service in `window.c` is deliberate:
that object already owns the current context, queue order, displayed metadata,
shuffle/repeat policy and player service. A parallel MPRIS state model would
eventually disagree with the UI.

Desktop media keys, shell media controls and compatible clients can play,
pause, stop, skip, seek, set volume, toggle shuffle and change repeat mode.
The service publishes playback status, microsecond position and duration,
album metadata, Spotify URI, CDN artwork URL and capability changes. `Raise`
presents the existing window and `Quit` exits the application. Property changes
and explicit seeks emit the standard MPRIS signals so clients do not need to
poll.

The current song is used as the native window title and as the MPRIS
`Identity`. This is an intentional compact-shell presentation choice: the KDE
task-manager media popup otherwise spends its heading on the static player name
and repeats title/artist beside the transport buttons. Title and artist are
therefore not duplicated in `Metadata`; the desktop entry remains the stable
application identity. The GTK title label keeps its full natural width but is
ellipsized, so it consumes available header space without increasing the
window's minimum width.

`OpenUri` is intentionally unsupported for now, and the advertised URI and
MIME-type lists are empty. Accepting arbitrary URIs without routing them
through the application's context/queue semantics would claim a capability the
player does not yet implement. Rate is fixed at `1.0`; setting any other rate
returns an MPRIS not-supported error.

Open or deliberately limited areas include:

- native search relevance differs from Spotify's consumer ranked search;
- playlist artwork upload;
- broader PipeWire validation;
- VA-API hardware JPEG decode in the Connect application;
- ad-insertion/feature-state research;
- Flatpak packaging;
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
then caused a retry loop. Target-sized C decode, bounded queues and ownership
guards fixed that loop. A later live profile exposed a second Library-specific
failure: GTK initially bound 257 cards whose temporary geometry made 241 look
visible. Album-card selection therefore uses stable model positions and the
scroll adjustment. Wrapped Library and Playlist grids calculate explicit
column and row counts, retain the visible rows plus one complete overscan row,
and warm the eased wheel destination row. Horizontal shelves keep a separate
one-dimensional calculation. Provisional GTK geometry is clamped to the
physical card-height floor; without that floor the first allocation advertised
fictitious 10-20px rows and reproduced the 241-cover/100 MiB spike. Widget
`mapped` state and pre-layout bounds are not viewport authority.

Track-list velocity overscan has a separate destination rule for eased wheel
gestures. Moving one contiguous ownership window on every animation frame
decoded every transient row crossed: one live session issued 541 cover reads,
completed 426 and cancelled 115 while retaining barely a megabyte of visible
pixels. Released textures stopped being logically live, but decoder and
renderer arenas retained their high-water allocation and later scrolling
stuttered. Wheel overscan now warms only the landing viewport plus eight rows,
keeps already-painted rows while they remain visible, and never starts a cover
decode solely because an intermediate row passes through the viewport. A
14-notch replay issued 59 incremental reads with a peak queue of three and
changed RSS by only 0.6 MiB.

### Mouse-wheel behavior

Touchpad scrolling was already smooth because GTK received continuous deltas;
mouse wheels needed a capture-phase target animation. Anchor corrections and
integer-pixel quantisation require explicit ownership and completion rules. In
one reproduced failure a gesture remained four pixels from its fractional
target for more than 1,100 frames, keeping the renderer active and making later
scrolls stutter. The animation now lands within an eight-pixel perceptual
tolerance, terminates after three no-progress frames, and logs only genuine
long frame stalls rather than doing diagnostic I/O on ordinary missed frames.
Elapsed-time easing fixed oscillation and frame-rate-dependent stutter. The
current slider exposes the tuned response without touching touchpad kinetics.

### Library/Playlist timeout and cache loss

Mercury content-type contamination caused request timeouts. Repeated READY
events then reset page models on reconnect, making names/media appear uncached.
Dispatch snapshots and same-session guards address the two independent causes.

### Album-grid 100 MiB spike

Logs separated visible texture memory from queued decode buffers and stale
widget references. Deferring work until layout, refusing duplicate card jobs,
and removing global cover deferral fixed the fan-out and cross-page race.

### Windows portable sign-in certificate failure

The first bundled Windows build contained the TLS DLLs but not a trust store.
The browser portion of OAuth worked, so the failure looked like a callback or
state bug; the log showed the decisive `Unacceptable TLS certificate` during the
POST to `accounts.spotify.com/api/token`. MSYS2's GnuTLS backend resolves its
system CA path from the build installation, which is not a valid path after a
`dist/` directory is copied to another machine.

The fix stays in the existing runtime/log module (no new C or header files):
the app finds the executable with `GetModuleFileNameW`, sets GIO, GSettings and
GdkPixbuf module paths relative to it before GTK starts, and loads one
process-lifetime `GTlsFileDatabase` from `etc/ssl/certs/ca-bundle.crt`. Every
libsoup session is configured with that database, including native OAuth,
client-token/login5, AP discovery, Pathfinder/catalog, artwork, CDN and
Connect. Verification remains enabled; there is no insecure “accept any
certificate” fallback. The Windows bundle script copies both the GIO module
cache and the CA bundle, and the optional libsoup probe runs from inside
`dist/` to verify the relative lookup and a real HTTPS handshake.

## Related documents

- [Root README](../README.md) — user-facing requirements and quick start.
- [Internals research](../research/internals.md) — subsystem status and live
  validation details.
- [Connect research](../research/connect.md) — Connect protocol notes.
- [Windows README](../apps/spotify-native-windows/README.md) — Windows build.
- [Third-party licenses](../THIRD_PARTY_LICENSES) — attribution obligations.

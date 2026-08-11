# Internals

Architecture, current status per subsystem, and measured performance. Moved
out of the README so that stays a description of what the program is rather
than of how it is put together.

## Architecture

```
┌───────────────────┐   ┌──────────────────────────────┐
│  spotify-native   │   │  spotify-native-harness      │
│  (GTK4 shell,     │   │  (src/main.c, CLI + probes)  │
│   src/ui/)        │   └──────────────┬───────────────┘
└─────────┬─────────┘                  │
          │                            │
┌─────────▼──────────────┐             │
│  SpotifyNativeSession  │             │
│  (src/spotify/         │             │
│   session.c)           │             │
│  Worker thread; holds  │             │
│  AP login → client-    │             │
│  token → login5 open,  │             │
│  serves catalog reads  │             │
└─────────┬──────────────┘             │
          │                            │
          └────────────┬───────────────┘
                       │
┌──────────────────────▼────────────────┐
│           Protocol layer               │
│           (src/spotify/)               │
│   Shannon cipher (real), AP framing,   │
│   Mercury pub/sub, audio key exchange, │
│   CDN chunk fetching, spclient         │
│   (context-resolve + extended-metadata)│
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

## Status by subsystem

Being direct about what's implemented vs. scaffolded, because a stream cipher
or DH handshake that *looks* done but is subtly wrong is worse than an honest
gap:

| Module | Status |
|---|---|
| Shannon cipher (`spotify/shannon.c`) | ✅ Confirmed against real ground-truth test vectors (a compiled-and-run copy of the actual reference crate, not read-and-reasoned-about). Note: the internal round-trip self-test alone had passed for a while despite a real bug (`sbox1`/`sbox2` used XOR where the reference uses OR) — that class of test can't catch a deviation applied symmetrically to both encrypt and decrypt. See `research/auth/` for the full writeup. |
| Ogg/Vorbis decoder (`audio/decoder.c`) | ✅ Confirmed against a live decrypted Spotify CDN range: 44.1 kHz stereo, 86,592 PCM frames from the initial 64 KiB probe. |
| Audio output: PulseAudio, ALSA | ✅ Functional; PulseAudio live-validated with 86,592 decoded Spotify PCM frames. |
| Output volume | ✅ Applied in software, in the audio worker. Every backend declares `.set_volume = NULL` — Pulse, ALSA and PipeWire all leave per-stream volume as a TODO — so routing the UI slider to `spotifygtk_output_set_volume()` reached a stub and silently did nothing. Scaling the PCM buffer works on every backend and needs nothing from them, which is the same pure-C fallback rule the rest of the project follows. The curve is cubic: perceived loudness is roughly logarithmic, so a linear slider sounds loud for most of its travel and then falls off a cliff. |
| 15-band graphic equaliser (`audio/dsp.c`, `ui/eq_graph.c`) | ✅ Functional. RBJ-cookbook peaking biquads at ISO 2/3-octave centres (25 Hz–16 kHz), ±12 dB per band, applied in the audio worker after volume and before output — the same push-from-the-UI path volume uses. Q tracks the spacing (2.145); the earlier ten octave-spaced bands used √2. Enabled-but-flat and disabled both short-circuit to a byte-exact no-op; gains persist in `settings.ini`. Unit-tested in `tests/test_dsp.c`; pure PCM DSP, so no live-server dependency. The UI is a drawn response curve, not a slider strip: `spotifygtk_eq_response_curve()` evaluates the real cascade magnitude (coefficients computed once per curve, curve cached until a gain or the width changes — recomputing per pixel per frame was visibly laggy), and the handles are dragged on the curve itself. Sampled every four pixels rather than every one: the response is smooth on a log axis, and a thousand-segment path stroked twice per redraw with round joins was most of what remained of the lag. The neutral tones are taken from the theme, since literal black reads as deliberate on a dark page and as a muddy slab on a light one. |
| Sample-rate resampler (`audio/resampler.c`) | ✅ Functional, unit-tested; **not yet listened to.** Polyphase windowed-sinc (Kaiser β 8.6, 32 taps, 256 phases, linear phase interpolation). Downsampling moves the cutoff below the output Nyquist so nothing folds back; each phase row is normalised so DC does not ripple as the phase walks. Equal rates are a byte-exact passthrough and remain the default — converting 44.1 kHz cannot add information, and the short path is better unless something downstream would resample anyway. Wired end to end: the Sample rate setting reaches the engine control, the audio worker opens the device at that rate and converts each block, and position reporting follows the device rate. Five offline tests in `tests/test_resampler.c` cover passthrough exactness, frame-count ratio, tone level, DC stability, and rejection of a tone above the new Nyquist. |
| Audio output: PipeWire | 🟡 Implemented, needs validation against a running PipeWire instance |
| Audio output: WASAPI (Windows) | 🟡 Implemented, cross-compile verified, **never run**. Shared-mode `IAudioClient` + `IAudioRenderClient`, blocking write for backpressure, `ISimpleAudioVolume` when the endpoint offers it, COM initialised on the audio worker that also tears it down. Opened with `AUTOCONVERT_PCM` so a 44.1 kHz stream still opens on a 48 kHz mixer, retrying without the flag for drivers that reject it. Compiles **and links** into a Windows `.exe` against real mingw-w64 headers with `-Wall -Wextra` and no warnings, which confirms the `COBJMACROS` forms and that `INITGUID` resolves the CLSIDs/IIDs without a uuid library — but compiling is not running. See `apps/spotify-native-windows/README.md`. |
| AP handshake (`spotify/ap.c`, `dh.c`, `handshake_crypto.c`, `protobuf_min.c`) | ✅ Confirmed working against a real Spotify server — DH exchange, RSA server-signature verification, and HMAC-SHA1 key derivation all checked out. |
| AP login (`spotify/ap.c`, `spotify/native_auth.c`) | ✅ Confirmed working against a real Spotify server. Four real bugs found and fixed on the way here, each caught by a live server rejecting the previous fix rather than by any test that existed at the time: wrong client_id/scopes, a wrong protobuf field number plus missing required fields in the login message, a Shannon cipher bug (the actual root cause underneath both) that broke every encrypted packet regardless of content, and — found after login was already succeeding — a wrong field number for `canonical_username` that silently dropped it from every login. |
| Mercury protocol (`spotify/mercury.c`) | ✅ Confirmed live, both directions. The receive half did not exist — requests were written and never answered — so responses, subscriptions and pushed events are all new. Wire format is `[u16 seq_len][seq][u8 flags][u16 part_count]` then length-prefixed parts, part 0 being a protobuf header (`uri`, `content_type`, `method`, `status_code` as zigzag `sint32`, `user_fields`). Two limits found by measurement rather than documentation: a request ceiling of 16,384 bytes, and no server-side reassembly of fragmented *requests* — so oversized ones are refused locally with a 413 to the callback instead of vanishing. |
| Audio key exchange (`spotify/audio_key.c`) | ✅ Confirmed against a live AP session; retrieves a usable 16-byte AES key. |
| Streaming auth-relay (`spotify/clienttoken.c`, `spotify/login5.c`, `spotify/spclient.c`) | ✅ Confirmed live: reusable AP credential → client-token → login5 bearer → track metadata → CDN URL resolution. `spclient.c` still uses librespot's hardcoded fallback host rather than full `apresolve`. See `research/playback/`. |
| CDN fetch + AES-CTR decrypt (`spotify/cdn.c`) | ✅ Confirmed live for the initial range and an independent non-block-aligned range; plaintext begins with `OggS` and decodes to PCM. |
| Gapless playback (`audio/sink.c`) | ✅ Confirmed by listening. The output device used to belong to a track: `LiveTestState` is a stack local in the per-track run, it opened a device on the first decoded frame and drained and closed it on the last, and the next track only started once all of that had unwound. The silence between tracks was three things — the drain waiting for the device buffer to empty, the close, and then a cold start of the next track before a device could be opened again. The sink lifts the device and its writer above any single track and holds them open; tracks queue in submission order, each with its own frame queue, and the writer moves to the next when the current is ended and empty. A run now finishes when it has pushed its last frame, several seconds before that audio plays, and those seconds are the head start the next track gets — so prefetch falls out of the ordering rather than needing machinery of its own. A track that decoded at a different rate is resampled to the device rather than reopening it. Because the producing track and the audible one are no longer the same, transport commands and position are routed to whichever the sink is actually sounding, and the UI defers adopting a track until the handover. |
| Native playback worker (`native_engine.h`, `player_service.c`) | 🟡 Runs the validated pipeline off the GTK thread, reports CONNECTING/BUFFERING/PLAYING/PAUSED stages, feeds decrypted CDN ranges into the audio sink (see the row above; it used to own a bounded PCM queue and its own output worker), propagates cancellation, and supports pause/resume, volume, and switching tracks mid-playback. Two bugs worth remembering: `start_uri()` used to return `G_IO_ERROR_BUSY` whenever anything was already playing, so picking a second track did nothing at all; and `pause()` emitted `PLAYING`, which left the bar showing a pause icon while paused, so the next click paused again and resume was unreachable. **Playback position is now reported**: the audio worker publishes cumulative frames-written onto the engine control, `player_service` polls it 4×/second and emits `position-changed`, and the bar's progress fill and time labels track the playing track (paused/stalled audio holds position, since only device-written frames count). **Seek now works**: the engine walks Ogg pages, replays the cached id/comment/setup header into a fresh decoder, and fetches a landing window at the requested byte offset, so the progress slider is draggable (debounced, pause-state preserved across the seek). **Stability fix (this pass):** the UI cancellable was only *checked* at the pause/queue waits and threaded into the CDN fetcher — it never quit the engine's main loop — so a stream that stalled with no async op in flight left the worker running forever, freezing the timestamp and making every later track unplayable until a restart. A `g_cancellable_source_new` on the engine context now quits the loop on Stop/track-switch, so the worker unwinds and the next track plays. **Thread-affinity fix (root cause of the freezes and the SIGSEGVs):** the AP reconnect retry was scheduled with `g_timeout_add()`, which attaches to the *global default* context — so the retry fired on the GTK main thread, and everything chained off the reconnect (client-token, login5, spclient, every CDN range) bound to the main context too, because libsoup/GTask capture the thread-default at call time. Core dumps showed `on_range_response` running under `g_application_run` with `stream_queue_frame` blocked in `g_cond_wait_until` — i.e. the UI thread waiting on the audio queue — and the engine state machine running on two threads at once, freeing a socket client under the other (SIGSEGV in `g_object_ref`). Both retry sites now attach a `GSource` to their own context. The retry path is routine (a fresh AP connection per track), which is why it fired so often. A play queue and Previous/Next exist as a *UI* layer over this one-track-at-a-time engine (`ui/window.c`): the window holds the current list as a play-context plus a user queue and hands the engine the next URI when a track finishes (natural completion arrives as `IDLE`; a user-driven switch never does, so the two never collide). |
| Spotify Connect registration (`spotify/connect.c`) | 🟡 Mercury subscription real, device-state payload pending real protobuf schema; ad-insertion events not yet researched |
| Image cache VA-API hardware decode | 🟡 Probe works, decode path stubbed (lives in `spotify-connect`, shared concept) |
| Vulkan compositing | ⬜ Not started |
| GTK4 UI shell (`gui_main.c`, `ui/`) | 🟡 libadwaita shell (`ui/`, built when libadwaita ≥ 1.4 is present; `gui_main.c` remains the fallback), laid out to a supplied reference design: `GtkHeaderBar` chrome, icon sidebar with pill selection and a Pinned section, three-column playback bar with the progress row under the transport, and a Now Playing panel whose cover is aspect-locked to the panel width via `GtkAspectFrame`. **Search and Liked Songs run entirely on the native protocol stack** — confirmed in the running GUI, which signs in via the session and renders real tracks with no Web API call. **Home, Search and Library are now populated**: each shows the distinct albums present in a resolved track list (search results, or the Liked Songs collection), as cards that open the real album via the same `context_page` — real data grouped from the tracks, not invented, since the native stack has no album-listing endpoint. Playlists have their own page of cards, and only Recently Played — which has no known endpoint — stays empty and says why. Previous/Next are live (backed by the UI play-context and queue), as is a right-click track menu — Add to Queue, Add to / Remove from Liked Songs, Add to Playlist, Go to Album, Go to Artist — where album/artist open the shared `context_page` resolved from URIs `track_meta` extracts from each track's Album/Artist gid (confirmed live: every collection and search track came back with valid 22-char base62 album+artist URIs). Also present: a 15-band equaliser in Settings, browser-style Back/Forward header navigation over a page-history stack, a local Liked Songs filter, a frosted (gradient) search header the results scroll under, and a marquee Now Playing title that scrolls when long instead of forcing the panel off-screen. A **login gate** covers the whole window until the session reports READY (a stored token is not proof it works) and returns on FAILED, with **Log out** under Settings → Account clearing both the in-memory tokens and the stored token file. The album shelves and grid are virtualised (`GtkGridView` / horizontal `GtkListView`): the first version built one card per album, and since each card's `GtkImage` holds its own texture reference, several hundred cards pinned ~75 MB of covers the bounded cache could not reclaim. Liked Songs has a reversible three-way sort (date added / length / A–Z); date-added sorts on the collection's own newest-first ordering, since the `added_at` timestamp lives in `collection2v2` and cannot yet be read. An album's release year sits beside its title, parsed from `Track.album.date`. Every scroller animates the wheel toward a target rather than stepping, so the wheel and the scrollbar move alike. **Liking is wired throughout**: a heart on every row aligned to the duration column, in the context menu with the label following the current state, and on the playback bar as a real control. The set of liked URIs is shared by every list rather than copied per page, and is rebuilt into a second table and swapped in, so a re-read never blanks the hearts while it runs. A like updates the row, the set and the Liked Songs page immediately and splices a single row rather than rebuilding the list — on a 5,000-track library a rebuild threw the user back to the top. Still disabled: shuffle and repeat. Settings carries an **About** section with the repository link and the date this copy was first run. The duplicate Settings/Notifications icons that used to sit on the Home header were removed — the sidebar's Settings item is the single working entry point. |
| ⚠️ Search relevance | 🟡 Known limitation. `/context-resolve/v1/spotify:search:<q>` is **not a search-results endpoint** — it returns a *playback context*, i.e. what Spotify would queue if you hit play on that search. "bohemian rhapsody" comes back with Blinding Lights first and the Queen tracks at #2–4, then Hotel California and Billie Jean. librespot documents the same behaviour ("massively influenced by the provided query") and implements no real search. The search page applies a client-side relevance filter on title and artist as a mitigation, falling back to the unfiltered list if nothing matches. A proper fix needs Spotify's actual search API, which is not in librespot and has not been identified. |
| ⚠️ `gui_main.c` fallback | 🔴 Still calls `api.spotify.com` with the keymaster token, so the non-libadwaita build has the shared-quota 429 problem described above. Only the `ui/` shell was migrated. |
| ⚠️ Catalog data path | ✅ **Migrated off `api.spotify.com`.** The native shell used to mint a token from the keymaster `client_id` (`native_auth.h`) and send it to `api.spotify.com`. Spotify meters that host per `client_id`, and that one is Spotify's own internal ID — shared by every librespot user — so every request drew on a globally-contended quota and returned `429 API rate limit exceeded`. Diagnosed by elimination: no token and a garbage token both return `401` from the same host, so a 429 required a *valid* token on a throttled `client_id`, and `Retry-After` moved independently of our own request rate. librespot never calls `api.spotify.com` at all (zero references in its repo). Catalog reads now go over spclient. |
| Artist visuals — **GraphQL** (`spotify/spclient.c`) | ✅ **This client calls Spotify's GraphQL API for one thing: the artist banner.** It is not in the native protocol — the `Artist` descriptor has no header field across all eighteen, the full `ExtensionKind` enum has no visuals kind, and the shipped binary carries no `hm://` artist route. It comes from pathfinder (`api-partner.spotify.com`), persisted query `queryArtistOverview`, operation and hash **probed out of the shipped client's bundle**. Two things had to be right and neither was: the endpoint answers `403 "Client/request not allowed"` to any non-browser **User-Agent** — not the client id (the official client uses the same keymaster id we do), not the client token, app version, platform, origin, or method, all of which were varied against a live server — and the field is `artistUnion.headerImage`, **not** `visuals.headerImage`; `visuals` holds the round avatar and the promo gallery. Because a missing banner is legitimately not an error, the broken call fell back to the avatar and looked like it worked for months. Sources are ~2660×1140 and `i.scdn.co` serves the ids, so the cover loader is unchanged. See `apps/spotify-native/research/artist-images.md`. |
| Reusable native session (`spotify/session.c`) | ✅ Confirmed live. Holds the AP login → client-token → login5 chain open instead of tearing it down per playback attempt, and serves catalog queries against it. Runs its own worker thread and `GMainContext` — the same isolation `run_live_test()` uses, so protocol callbacks can never dispatch on the GTK thread — while the public API is main-thread-facing and returns results via `GTask`. `load_tracks()` does both round trips (context-resolve, then batched metadata) because neither alone yields a renderable row. Written against the existing public modules rather than by refactoring `main.c`, so the playback harness is untouched. Renews the login5 bearer before it expires (it lasts 3600s); without that the session kept reporting READY while every request 401'd — a silent death about an hour in. Disposing a session mid-sign-in used to deadlock, because `dispose()` found a NULL loop, skipped the quit, then blocked in `g_thread_join` while the worker went on to run one; the loop is now published and claimed under the lock. `SPOTIFY_PROBE_SESSION=dispose` exercises that race directly, since SIGTERM cannot (the process dies without running `dispose`). |
| Track display metadata (`spotify/track_meta.c`) | ✅ Confirmed live. Extracts name/album/artist/duration/explicit — and now the Album/Artist `gid` (field 1), base62-encoded to `spotify:album:<id>` / `spotify:artist:<id>` so the row menu can navigate — from the `Track` message `spclient.c` already fetches for playback and previously discarded. The base62 encoder is cross-checked in `tests/test_track_meta.c` against an independent big-integer reference (not against itself), and confirmed live: every collection and search track resolved to valid 22-char ids. Field numbers transcribed from `metadata.proto` and asserted as literals in `tests/test_track_meta.c`. **`duration` is zigzag-encoded `sint32`** — an earlier revision of this file asserted the opposite in a comment, and reading it as a plain varint returned exactly double (Rick Astley's "Never Gonna Give You Up" as 7:07 instead of 3:33). Only live data caught it; the offline round-trip test had happily agreed with the wrong encoder. Now pinned by a regression test using real track lengths. |
| Context resolution (`spotify/spclient.c`) | ✅ Confirmed live. `/context-resolve/v1/{uri}` — the single endpoint behind search, liked songs, and playlist/album/artist listings; returns JSON, unlike the rest of spclient. Search for "never gonna give you up" returned 20 correct tracks; `spotify:user:<id>:collection` returned a 4,773-track library. **`ContextTrack` carries `uri` and nothing else** — no title, artist, or metadata map at all, so this is a URI list and every rendered row requires the batched-metadata call below. URI builders are unit-tested for escaping so a query cannot inject URI structure (`tests/test_context_uri.c`, 9 cases). |
| Batched display metadata (`spotify/spclient.c`) | ✅ Confirmed live. `BatchedEntityRequest.entity_request` is repeated, so a whole page resolves in one round trip. Verified at 50/200/500/1000/2000 URIs; a single request for all 4,773 collection tracks fails, so the ceiling is between 2000 and 4773. The catalog pages now **page**: a load is split into 2000-URI requests and concatenated, so a whole collection arrives (confirmed live at 4,796 tracks across three pages, and since at 5,236) rather than being truncated to one batch. Results correlate via `EntityExtensionData.entity_uri` rather than request ordering — and until 2026-08-02 nothing performed that correlation, so results were appended in whatever order they came back and every context lost its ordering. Albums arrived shuffled. Tracks are now placed by request position, which matters most where it is least recoverable: records that cross-fade between tracks. |
| Live probe (`main.c`, `SPOTIFY_PROBE_CONTEXT`) | ✅ Env-gated diagnostic that runs the full chain (AP login → client-token → login5 → context-resolve → batched metadata) and prints what the UI would render. `SPOTIFY_PROBE_CONTEXT="some query"` searches, `="collection"` lists liked songs, or pass any `spotify:...` URI; `SPOTIFY_PROBE_LIMIT` caps the batch. Replaces playback rather than preceding it, so a diagnostic failure can't be confused with a pipeline failure. |
| Playlists (`spotify/playlist.c`) | ✅ Confirmed live: list, create, and add. The rootlist parses (`playlist4_external`) and drives a Playlists page of cards. Three things about this service differ from the collection one and are easy to get wrong: it takes **POST** where the collection takes only PUT; a created playlist is **not** in the library until it is separately filed into the rootlist, so a create returning 200 still shows nothing; and changes are **revision-checked**, so every write is read-then-write against the head. `ListItems.items` is field **3** — field 2 is `truncated`, and the wrong number parses cleanly and finds nothing. `Add.from_index` and `Add.add_last` are alternatives; sending both is a 400. Cards resolve their name and cover lazily, when scrolled into view, because the rootlist carries neither and doing all of them up front cost two round trips per playlist. |
| Liked Songs read/write (`spotify/collection.c`) | ✅ Confirmed live. The real API is `POST hm://collection/v2/{initialized,paging,delta,write}` with content-type `application/vnd.collection-v2.spotify.proto`; the older `hm://collection/collection/<user>` is legacy and its PUT **replaces the entire set** rather than adding to it. That distinction is not academic — treating it as additive wiped a 4,806-track library during development, recovered only from Spotify's own backup. Destructive probes are now gated behind `SPOTIFY_COLLECTION_WRITE_OK` naming the account. Writes are additive and name only what changed; a like made while the connection is down is queued and replayed on reconnect. |
| Listening history | ⬜ Blocked upstream — no spclient endpoint for recently-played is known (librespot exposes none), so Home's "Recently Played" has nothing to port. |

## Performance

Measured on a 5,236-track library, since that is where everything that scales
badly shows up first.

| | |
|---|---|
| Sign-in to a loaded collection | ~3.8s. Playlists are no longer resolved at sign-in; they cost one head read and one cover resolve each, and doing all of them up front put the collection load behind 59 serialised round trips. |
| Opening Playlists | cards appear in ~0.3s and fill in as each resolves. |
| Concurrency | `spclient` sets its own connections-per-host (8). libsoup's default is what made per-entry lookups serialise; Mercury was never the bottleneck, since it carries many requests at once keyed by sequence number. |
| Memory | ~265 MB RSS with the library loaded, down from ~380 MB before covers were cached on disk. Note that ~163 MB of that is Mesa's `libLLVM`/`libgallium` mapped read-only — an empty GTK4 window on the same machine is already 189 MB RSS — so RSS overstates the app's own share considerably. Cover textures dominate the heap and are held at the size they are drawn: decoding smaller made them visibly soft, which is a trade not worth making. What did work was caching the compressed images on disk, which let the in-memory budget drop from 48 MB to 24 MB, since evicting now costs a file read rather than a round trip. |

## Development switches

All are environment variables, all off by default.

| Variable | Applies to | Effect |
|---|---|---|
| `SPOTIFY_PROBE_SESSION="<query>"` | harness | Signs in via `SpotifyNativeSession` and prints the tracks a search would render. `="collection"` lists liked songs instead. |
| `SPOTIFY_PROBE_SESSION=dispose` | harness | Tears down sessions mid-sign-in to exercise the shutdown race. Hangs on regression; SIGTERM cannot test this, since the process dies without running `dispose`. |
| `SPOTIFY_PROBE_CONTEXT="<query>"` | harness | Runs the raw chain (AP login → client-token → login5 → context-resolve → batched metadata) and dumps the response shape. Replaces playback, so a diagnostic failure can't be confused with a pipeline failure. |
| `SPOTIFY_PROBE_LIMIT=<n>` | harness | Caps how many URIs the batched-metadata probe resolves. |
| `SPOTIFY_DEV_START_PAGE=<name>` | GUI | Opens straight onto `home`, `search`, `liked`, `library`, `playlists` or `settings`, so a page's load can be watched without driving the UI by hand. |
| `SPOTIFY_DEV_INSTANCE=1` | GUI | Runs non-unique, so a freshly built copy can launch while another is already open. |
| `SPOTIFY_COVER_CONNS=<n>` | both | Connections per host for cover art (default 6). |
| `SPOTIFY_SPCLIENT_CONNS=<n>` | both | Connections per host for spclient. Default 8; this is what stops per-entry catalog lookups from serialising. |
| `SPOTIFY_COLLECTION_WRITE_OK=<user>` | both | Interlock for destructive collection probes: they refuse to run unless the signed-in user matches. Added after a probe wiped a real library. |

There are many more `SPOTIFY_PROBE_*` variables in `main.c`, each gating one
protocol experiment; `grep -rho 'g_getenv ("SPOTIFY[A-Z_]*"' src/` lists them.

Diagnostics are compiled out by default and enabled with a build option:

```bash
meson setup build -Dverbose_logging=true   # adds the [dbg] per-operation traces
```

The app also writes `build/src/spotify-native.log`, rotating the previous run
to `spotify-native.prev.log`.

**Verifying a build actually contains your change.** Meson/ninja has been
seen to leave a stale object file behind, producing a binary that links an
older translation unit while reporting a successful build — in one case the
executable still had a removed button compiled in. A green build is not
proof. Confirm with `strings build/src/spotify-native | grep <symbol>`, or
`ninja -C build -t clean && ninja -C build` before any visual check.

## Audio backend tracks

```bash
meson setup build --native-file build-profiles/stable.ini   # Pulse/ALSA
meson setup build --native-file build-profiles/nightly.ini  # + PipeWire
```

## Core principles

- **No dependency younger than 5 years** for any critical path
- **Every external library is optional**, with a pure-C fallback
- **Runtime probing over compile-time feature gates** — the binary works
  everywhere, it just silently takes the best available path

## Roadmap

- [x] Reorganize into `apps/spotify-connect/` + `apps/spotify-native/`
- [x] Port real Shannon cipher from librespot's `shannon` crate
- [x] Document real DH params, RSA server key, keyexchange.proto schema
- [x] Hand-roll `keyexchange.proto` encoder, wire DH handshake into `ap.c`
- [x] Test the handshake against a live Spotify server
- [x] Confirm AP login succeeds end-to-end with the corrected native_auth token
- [x] Confirm the streaming auth-relay chain (client-token → login5 → spclient) against real services
- [x] Extract the auth-relay into a reusable session object
- [x] Move the catalog off `api.spotify.com` onto spclient context-resolve + extended-metadata
- [x] Refactor the shell to the reference design
- [x] Album art end to end — cover IDs from `Track.album.cover_group` fetched directly from `i.scdn.co/image/<id>`, decoded at target size on a worker thread, cached as `GdkMemoryTexture` in a bounded per-(id,size) LRU (rows, panel, bar and album cards)
- [x] Populate Home / Search / Library with the distinct albums present in a resolved track list (cards open the real album; no album-listing endpoint needed)
- [x] 15-band graphic equaliser — RBJ peaking biquads in the audio worker, persisted in `settings.ini`, with a drawn (and draggable) real response curve
- [x] Sample-rate resampler — polyphase windowed-sinc, passthrough by default, five offline tests
- [x] Seek support in the engine, so the progress slider is draggable (Ogg page walking + header replay + landing-window fetch)
- [x] Like / unlike via the native collection service — `POST hm://collection/v2/write`, the real API rather than the legacy set-replacing one; queued and replayed if the connection is down
- [x] Parse `playlist4_external` — a Playlists page of cards, plus create and add-to-playlist
- [x] Mercury receive half, so requests are actually answered — responses, subscriptions and pushed events
- [x] Gapless playback — the audio device outlives the track, and the next one decodes while the current is still sounding
- [x] Page beyond the first batch of a large collection — 2000 per request, concatenated; verified at 5,236 tracks
- [x] Play queue + Previous/Next + right-click track menu (Add to Queue, Add to Playlist, like/unlike, Go to Album/Artist), as a UI layer over the engine
- [x] Browser-style Back/Forward header navigation over a page-history stack
- [x] Themes: Dark (default), White, Milk — one rule body over three `@define-color` palettes, applied live via a reloaded `GtkCssProvider` + `AdwStyleManager` color-scheme
- [ ] Identify Spotify's real search endpoint; context-resolve returns a playback context, not ranked results
- [ ] Port `gui_main.c` (the non-libadwaita fallback) off the Web API
- [ ] Ad-insertion / feature-state event handling (`research/connect/`)
- [ ] VA-API hardware JPEG decode path (probe works, decode pending)
- [ ] Vulkan compositing, UI for `spotify-native`
- [ ] Shuffle and repeat
- [ ] MPRIS2 D-Bus interface
- [ ] Flatpak packaging

**Optional / blocked on hardware or subscription:**

- [ ] **Widen the audio pipeline past 16-bit.** `PcmFrame.samples` is `gint16`
      and both output backends are configured to match (`PA_SAMPLE_S16LE`,
      `wBitsPerSample = 16`), so the whole path is 16-bit end to end. Worth
      doing on its own merits before any lossless work: the 15-band EQ and the
      resampler operate on these buffers, and biquad filtering in 16-bit
      accumulates quantisation that float would not.
- [ ] **FLAC decode** (`FLAC_FLAC = 16` in `AudioFile::Format`, already
      labelled in the metadata dump). Only reachable on Spotify's Platinum
      tier, which is regional -- available in India, not in Poland and
      elsewhere -- so this cannot be tested without a subscriber on a market
      that has it. Behind a Meson option until then (`meson setup build
      -Dplatinum=true`; a build flag, not a ninja one -- ninja only builds what
      Meson configured).

      Ordering matters: pipeline width first, FLAC second. Adding FLAC to a
      16-bit pipeline would decode losslessly and then truncate, which looks
      correct in every log and silently discards exactly what the tier is sold
      for. libFLAC itself is small, C, and packaged for MSYS2, so it fits the
      Windows build and the no-Rust constraint.
- [x] Windows port — builds under MSYS2 UCRT64 and **runs on real Windows 11**: sign-in, streaming, and WASAPI playback all confirmed, and the per-OS client-token block validated against live servers (HTTP 200). `apps/spotify-native-windows/` holds the toolchain files, build script and packaging; `build/dist/` runs portably on a machine with no MSYS2 and no GTK. Remaining: no installer, `build.sh --bundle` still walks the DLL closure in a single pass (loadable modules need a fixpoint), and the stored token is not DPAPI-protected — see that directory's README

---

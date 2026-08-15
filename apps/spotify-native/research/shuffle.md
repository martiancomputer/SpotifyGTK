# How Spotify shuffles, and what Smart Shuffle actually is

Recovered from the shipped Linux client, `/opt/spotify/spotify` (58 MB, dated
2026-08-06), by reading its embedded protobuf descriptors. Nothing here has
been run against a server yet — it is the client's own idea of the protocol,
which has been reliable ground truth for this project before, but it is not
the same as having done it.

## The short version

Two separate things get called "shuffle", and conflating them is why this
looked confusing from outside:

1. **Shuffle order** is a property of the *context*, not a local decision. The
   server hands the client a `ShuffleAlgorithm`, and one of its variants
   carries a **seed**. Order is derived, not rolled.
2. **Smart Shuffle** — the thing that mixes recommendations into a playlist —
   is not an ordering algorithm at all. It is a *mode*, gated on a device
   capability, and the extra tracks come from the server.

Our client rolls its own order locally (`rebuild_order()` in `window.c`). That
is why our shuffle cannot agree with the official client's, and never will
while it ignores the seed.

## ShuffleAlgorithm

`spotify.playback_platform.context.v1.behavior.ShuffleAlgorithm`, a oneof over
four variants:

| variant | fields | what it means |
|---|---|---|
| `Simple` | `algorithm` enum | `SIMPLE_UNSPECIFIED`, `SIMPLE_FORWARD`, `SIMPLE_SHUFFLE`, `SIMPLE_REVERSE` |
| `Stable` | `prescribed_pages` (repeated `Permutation`), `min_num_children` | the server dictates the exact order |
| `Format` | `permutations` (map to `Permutation`), `fallback` (`ShuffleAlgorithm`, recursive) | per-format order, falling back to another algorithm |
| `Burned` | `shuffle_seed` | order derived from a seed |

`Permutation` is a single repeated field, `indices`.

`Burned` is the interesting one. A seed on the context is what makes shuffle
*the same everywhere*: pick up on another device, or reconnect, and the order
continues rather than being reshuffled. It also explains why Spotify's shuffle
survives a client restart, which a locally rolled order cannot.

`Stable` and `Format` go further and let the server prescribe the order
outright, as explicit index permutations.

## ShufflePlacement

`spotify.playback_platform.context.v1.behavior.ShufflePlacement`, with
`partition_number`, `partition_layout` (`Layout`), `group_number`,
`group_layout` (`Layout`) and `uniform` (`Uniform`).

So ordering is not a flat permutation: tracks are partitioned and grouped, and
placement is decided per group. That is the machinery behind "shuffle does not
play three songs by the same artist in a row".

## The scorers

Named as remote-config flags rather than wire fields, so they are switches the
server holds, not something a client sets:

```
enable_artist_separation_shuffle_scorer      (…_for_all)
enable_track_separation_shuffle_scorer
enable_play_history_shuffle_scorer           (…_for_all)
enable_automix_shuffle_scorer
play_history_shuffle_scorer_context_track_count
play_history_shuffle_scorer_history_track_count
play_history_shuffle_scorer_use_context_presence_instead_of_context_uri
```

Artist separation, track separation, play history and automix are the ranking
inputs. Play history is scored against what has actually been listened to,
which is not information a client has in full.

## Smart Shuffle

Not an algorithm. The evidence:

- **`supports_smart_shuffle_mode`** appears in the device capability block,
  among `supports_dj`, `supports_rooms`, `supports_remote_sleep_timer`,
  `supports_ping_request` and `supported_audio_quality`. A client *advertises*
  it; it does not implement it.
- **`SetDisableSmartShuffleRequest`**, `DISABLE_SMART_SHUFFLE`,
  `disable_smart_shuffle` — the client's control over it is a single switch,
  and the switch is phrased as *disable*.
- **`sms_mode`** ("smart shuffle mode") is carried in state and events —
  `CoreShuffleStateEvent` has `cmd_sms_mode`, `state_sms_mode` and
  `playback_settings_sms_mode`.
- `PICK_AND_SHUFFLE`, `PickAndShuffleCapStateSave` / `…Restore` is the
  internal name for the pick behaviour, with its own saved state.

A `ShuffleAlgorithm` can only order the tracks that are already in the
context. Smart Shuffle adds tracks that are not in the playlist, so those
tracks must arrive with the context from the server. The client's role is to
say it supports the mode, turn it on or off, and report which mode it is in.

## What this means for us

- **Matching Spotify's shuffle order is possible in principle** — take the
  `shuffle_seed` off the context and derive the order from it instead of
  rolling one locally. That would make our order agree with the official
  client's, and survive a restart. It needs the seed's exact derivation, which
  is not in these descriptors: only the field is.
- **Implementing Smart Shuffle is not.** The recommendations are server-side
  and only sent to a device that advertises the capability during a session
  the server treats as eligible. There is nothing to compute locally.
- The nearest honest feature is a **better local shuffle**: artist and track
  separation are the two scorers whose inputs we actually have, and they are
  most of what makes Spotify's shuffle feel unrandom.

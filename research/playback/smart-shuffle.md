# Smart shuffle

Read-only probe of the official Linux client, 1:1.2.92.147-1
(`/opt/spotify/spotify` plus the CEF bundle `Apps/xpui.spa`). Nothing was
modified and no request was made; this is what the shipped binary and its UI
bundle say about themselves.

## What it is

**Smart shuffle is not an ordering the client computes.** It is a player option
the client sets and the server acts on. That is the whole finding, and it is
the reason it cannot simply be implemented locally the way ordinary shuffle
can.

From `xpui.spa`, `pip-mini-player-snapshot.js`:

```js
setShuffleMode (e, t, i) {
  ...
  if (i?.contextURI)
    return s = { shuffling_context: !!e,
                 modes: { context_enhancement: e === e6.SMART ? "RECOMMENDATION"
                                                             : "NONE" } },
           this._setOptions (s, t, i);
```

and the reverse mapping, which is how the UI decides which of the three states
to draw:

```js
function lR (e, t) {
  return e && t?.context_enhancement === "RECOMMENDATION" ? e6.SMART
                                                          : e6[e ? "ON" : "OFF"];
}
// e6:  0 = OFF, 1 = ON, 2 = SMART
```

So shuffle is a three-state control, not a toggle:

| state | `shuffling_context` | `modes.context_enhancement` |
|---|---|---|
| off | false | `NONE` |
| on | true | `NONE` |
| smart | true | `RECOMMENDATION` |

`NONE` and `RECOMMENDATION` are the only two values of the enum; both appear
verbatim in the core binary's string table alongside
`.spotify.playback_settings.esperanto.proto.ContextEnhancement`.

## Where the option lives

Two generations of the message exist, and only the newer one carries this.

`ContextPlayerOptionOverrides`, recovered from the binary's descriptors, is the
legacy shape and has no room for it:

```proto
message ContextPlayerOptionOverrides {
  optional bool shuffling_context = 1;
  optional bool repeating_context = 2;
}
```

The enhancement is carried in `modes` on the newer
`spotify.playback_settings.esperanto.proto` side, which is also where
`ContextEnhancement` is declared. The player-state snapshot the client renders
from carries `modes` through untouched:

```js
modes: A?.modes || {},
shuffle_mode: lR (A?.shuffling_context, A?.modes),
```

## Who produces the sequence

The server. The binary carries a whole proto family for it:

```
spotify/playback_platform/context/v1/context.proto
                                     requests.proto
                                     sequences.proto
                                     sequence_child.proto
                                     context_state.proto
                                     ledger.proto, ledger_entry.proto, ledger_query.proto
                                     behavior/{queue,skips,start}.proto
                                     metadata/basic.proto
```

`sequences.proto` / `sequence_child.proto` and the `ledger*` set are the shape
of a service that decides what plays next and remembers what it has already
handed out — which is exactly what an enhanced context needs, since the
recommendations have to be stable across skips and not repeat.

## What this means for this client

Ordinary shuffle is ours to do and is done: a permutation over the resolved
context, held beside it (`ui/window.c`). Smart Shuffle still cannot be computed
faithfully from `/context-resolve`; a probe of the complete collection response
confirmed that it returns only the original context pages.

Connect supplies the missing server-player path. The client now publishes
`shuffling_context=true` plus `context_enhancement=RECOMMENDATION` in its
`PlayerState`, and consumes the authoritative enhanced sequence when the
following cluster snapshot delivers it in `next_tracks`. Until that snapshot
arrives it retains the current order rather than presenting a locally invented
approximation.

There are two protobuf shapes to support:

- `ContextPlayerOptions` in transfers and player state uses shuffle field 1
  and modes field 5.
- `SetOptionsRequest` in remote commands wraps shuffle in `OptionalBoolean` at
  field 3 and puts modes at field 7.

The official apps can therefore initiate Smart Shuffle remotely, and the
native three-state control can initiate it by publishing the same Connect
state. `enhance_context` and `unenhance_context` in the JS bundle remain
telemetry names, not endpoints.

## Ruled out

- `enhance_context` / `unenhance_context` -- analytics event names only.
- `ContextPlayerOptionOverrides` -- legacy, cannot express it.
- Anything client-side in `xpui.spa` -- it sets the option and renders the
  resulting state; it never computes an order.

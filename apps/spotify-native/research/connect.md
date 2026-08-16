# What it takes to be a Spotify Connect device

`src/spotify/connect.c` registers a device today by sending a JSON blob over
Mercury to `hm://connect-state/v1/devices/<id>`. Its own comment says a real
`PutStateRequest` would be needed. That is right, and the gap is wider than the
comment suggests: the transport is wrong as well as the payload.

Recovered from `/opt/spotify/spotify` (2026-08-06) unless marked as measured.

## The shape of it

Three things, in order. None of them optional.

1. **A dealer connection.** A WebSocket to `wss://<dealer-host>/?access_token=…`.
   The server sends a message carrying a **connection id**; the client logs
   `ConnectConnectivityListener: received new_connection_id %s - starting`, and
   `received empty connection_id` is a named failure case. This is also the
   channel remote commands arrive on — transfer, volume, play.
2. **`PutStateRequest`, over HTTPS, to spclient** — not Mercury. The client has
   `ConnectStateClientImpl::putState: request failed with HTTP error %s`, a
   `PutStateBackoff`, and `allow_concurrent_put_states`. The path is
   `connect-state/v1/devices/<device_id>`, and the connection id goes in the
   **`X-Spotify-Connection-Id`** header.
3. **Reacting to `ClusterUpdate`** pushed back down the dealer, so the device
   follows what other clients do with it.

So the current code is wrong twice over: Mercury instead of HTTPS-to-spclient,
and JSON instead of protobuf — and it has no connection id to send at all,
because there is no dealer.

## The messages

`spotify.connectstate`:

```
Device       { device_info, player_state, private_device_info, transfer_data }
Cluster      { changed_timestamp_ms, active_device_id, player_state,
               device (map), transfer_data, transfer_data_timestamp,
               need_full_player_state, server_timestamp_ms, needs_state_updates }
ClusterUpdate{ cluster, update_reason, ack_id, devices_that_changed }
```

`DeviceInfo`, which is what registration is really about:

```
can_play, volume, capabilities (Capabilities), device_software_version,
device_type (spotify.connectstate.devices.DeviceType), spirc_version,
device_id, is_private_session, is_social_connect, client_id, metadata_map,
product_id, deduplication_id, selected_alias_id, device_aliases, is_offline,
public_ip, license, audio_output_device_info (AudioOutputDeviceInfo: brand,
model, audio_output_device_type)
```

`Capabilities` — the flags a device announces about itself. The server uses
these to decide what it may send and what a controller may offer:

```
can_be_player, is_controllable, is_observable, is_voice_enabled, hidden,
disable_volume, volume_steps, supported_types, command_acks,
supports_transfer_command, supports_command_request, supports_set_options_command,
supports_playlist_v2, supports_playlist_mixing, supports_smart_shuffle_mode,
supports_rename, supports_rooms, supports_dj, supports_hifi,
supports_lossless_audio, supports_external_episodes, supports_gzip_pushes,
supports_ping_request, supports_remote_sleep_timer, supports_set_backend_metadata,
supports_music_speed, supports_playback_speed, supports_zephyr
```

(Field numbers are not recorded here — the descriptors give names and order;
the numbers still have to be read out of the descriptor bytes before encoding.)

## What we already have

- **Measured:** `apresolve.spotify.com/?type=dealer` returns dealer hosts —
  `gae2-dealer.spotify.com:443` and three others. Our `apresolve.c` parses only
  the `accesspoint` array today, so this is a small extension rather than a new
  request.
- **libsoup 3.6.6** provides `soup_session_websocket_connect_async`, so the
  dealer needs no new dependency and no hand-rolled WebSocket.
- A bearer token already exists on the session (`bearer_token`, refreshed via
  login5) and is what the dealer URL wants.
- `spclient.c` already does authenticated HTTPS with bearer and client tokens,
  which is the transport `putState` needs.

## The gate is open — measured

**A third-party client can hold a dealer connection.** Run against the
throwaway on 2026-08-16: the WebSocket to `wss://gae2-dealer.spotify.com/`
with `?access_token=<bearer>` was accepted, and the dealer's first message
carried the id:

```json
{"headers":{"Spotify-Connection-Id":"NzY0ZDJkMGIt…QTNENg=="},
 "method":"PUT","type":"message",
 "uri":"hm://pusher/v1/connections/NzY0ZDJkMGIt…QTNENg%3D%3D"}
```

200 characters, base64. Two things worth knowing before writing the real
client:

- **Take it from the header, not the uri.** Both carry it, but the uri's copy
  is URL-encoded — the trailing `==` arrives as `%3D%3D` — so it would have to
  be decoded back. The header's copy is already what
  `X-Spotify-Connection-Id` wants.
- The bearer alone was enough. No client token, no prior registration, no
  device announcement.

So nothing about Connect is gated on being the official client, at least at
this step. `SPOTIFY_PROBE_DEALER=1` reruns it.

## Suggested order, each step provable on its own

1. ~~Extend `apresolve` to return the `dealer` list.~~ Done — the request asks
   for both types and `spotifygtk_apresolve_parse_type()` reads either.
2. ~~Open the dealer WebSocket and log the connection id.~~ Done, above.
3. **Half done.** The encoding is accepted; the device does not yet appear.

## Step 3, as far as it got — measured

Field numbers were read out of the descriptor bytes rather than assumed, which
mattered: `MemberType.CONNECT_STATE_EXTENDED` is **5**, not the 3 its position
suggests, `DeviceInfo.name` is **3**, and `brand`/`model` are **14**/**15** on
`DeviceInfo` itself rather than on `AudioOutputDeviceInfo`. The numbers came
from parsing the embedded `FileDescriptorProto` at `0x877f10`
(`devices.proto` at `0x87b230`).

```
PutStateRequest  device=2  member_type=3  is_active=4  put_state_reason=5
                 message_id=6  client_side_timestamp=12
Device           device_info=1  player_state=2  private_device_info=3
DeviceInfo       can_play=1 volume=2 name=3 capabilities=4 device_software_version=6
                 device_type=7 device_id=10 client_id=13 brand=14 model=15
Capabilities     can_be_player=2 volume_steps=8 supported_types=9
                 is_controllable=16 supports_transfer_command=19
                 supports_command_request=20 supports_smart_shuffle_mode=33
MemberType       SPIRC_V2=0 SPIRC_V3=1 CONNECT_STATE=2 CONNECT_STATE_EXTENDED=5
PutStateReason   SPIRC_HELLO=1 NEW_DEVICE=3 PLAYER_STATE_CHANGED=4 NEW_CONNECTION=9
DeviceType       COMPUTER=1 TABLET=2 SMARTPHONE=3 SPEAKER=4
```

A 121-byte `PutStateRequest` PUT to
`https://gae2-spclient.spotify.com/connect-state/v1/devices/<id>`, with the
dealer's connection id in `X-Spotify-Connection-Id`, returns **HTTP 200** and
~35 KB of `Cluster`. So the body parses — a malformed one answers 400 with an
empty body, which is the failure mode the Windows client-token block had.

**The device registers.** The cluster that comes back contains it:

```
device  name='SpotifyGTK'  id=fab2037c…6add  can_play=1
```

That took five rounds to see, because the check was broken rather than the
code. The probe asked `g_strstr_len()` whether the device name appeared in the
response — but the response is 35 KB of protobuf, full of NUL bytes, with the
device entry near the end. `g_strstr_len` is for strings and stops at the first
NUL, so it answered "absent" every time while registration was working
perfectly. `memmem` finds it immediately.

Everything "ruled out" along the way was therefore ruled out against a
verifier that could not have said yes: the device id format, the missing
player state, the missing client id, the late merge, the dead connection. Two
of those turned into real findings anyway — the dealer heartbeat is
client-driven `{"type":"ping"}` → `{"type": "pong"}`, confirmed on the wire —
but none of them was ever the problem.

Parse the cluster rather than searching it: `Cluster.device` is field 4, a map
whose value is a `DeviceInfo`, and `name` is field 3 of that.

## Why it may still not show on your phone

The device appears in the cluster **of the account it registered under**. A
phone signed into a different account has a different cluster and will never
see it, which is not a bug and cost some confusion here.

The PUT response and the dealer are both saying nothing, and they may both be
the wrong place to look.

`SPOTIFY_PROBE_DEALER=1 SPOTIFY_PROBE_PUTSTATE=1` reruns it;
`SPOTIFY_PROBE_CLUSTER=1` additionally dumps what came back.
4. Handle `ClusterUpdate` so remote control actually works.

Steps 1 and 2 are worth doing before any of the encoding: they answer whether
this is possible for a third-party client at all, which is not yet known.

## Step 4 is smaller than it looks: the updates already arrive

Measured on the main account, 2026-08-16. While registered and pinging, the
dealer socket received three messages of ~38 KB each, around the times a phone
tried to connect to us:

```
[dealer] message (38392 bytes): {"payloads":["CpTgAQjTt7TegDQa3MkBCKC6..."]}
```

The payload is base64 inside a JSON envelope, and decoding it gives exactly
what the field numbers predict:

```
field 1  ClusterUpdate.cluster            28692 bytes
  field 1  Cluster.changed_timestamp_ms   1786904386515
  field 3  Cluster.player_state           25820 bytes -> spotify:track:4ps9w...
```

So the server pushes full cluster state, including the *other* device's
`PlayerState`, down the socket we already hold open. `on_dealer_message()`
logs the first 700 characters and discards the rest.

That means step 4 is not "get the server to talk to us" -- it already does.
It is:

1. ~~Parse the `ClusterUpdate`.~~ Done. Base64 out of the JSON `payloads`
   array, then the protobuf; `active_device_id`, `context_uri` and
   `is_playing` come out.
2. ~~Notice when `Cluster.active_device_id` is ours.~~ Done — it logs
   `TRANSFER` when the cluster names this device.
3. Answer the command. **Built, untested against a phone.**

### A transfer is a command, not a cluster update

Measured when a phone tapped this device in its picker:

```json
{"message_ident":"hm://connect-state/v1/player/command",
 "payload":{"message_id":201102425,
            "sent_by_device_id":"cc384d544fe83dd273e264c23d0ccf9de47b046e",
            "command":{"endpoint":"transfer","data":"CgYIABAAGAASQAiMpbrfgDQ…"}}}
```

Two things that matter. It is `"payload"` **singular, an object** -- cluster
updates use `"payloads"`, an array of base64 -- so a parser written for one
shape silently ignores the other. And the cluster updates arriving alongside
said `active_device_id = (none)`: the controller does not make this device
active on its own. It waits to be answered.

The answer is a put_state that claims active and names the command:

```
is_active                      = 1
put_state_reason               = PLAYER_STATE_CHANGED (4)
last_command_sent_by_device_id = the command's sent_by_device_id  (field 7)
last_command_message_id        = the command's message_id         (field 8)
```

Those two fields are how a controller learns its command was taken. Without
them it shows "Connecting…" and gives up, which is what it did.

### Registration is not a one-off

A device that registers and then stops reporting state is dropped. The symptom
is unmistakable once seen: it appears in the picker, vanishes after a few
seconds, comes back once, then stays gone -- the "comes back once" being a
single delayed re-announcement, and nothing after it.

`put_state` therefore runs on a thirty-second keepalive, matching the ping.
A real client also puts state on every change, which comes with having state
worth reporting.

4. **Play what was transferred. Built.** `command.data` is base64 carrying a
   `TransferState`:

```
TransferState  playback=2  current_session=3
Playback       timestamp=1  position_as_of_timestamp=2  is_paused=4
               current_track=5 (ContextTrack)
Session        context=2 (Context)      Context  uri=1
ContextTrack   uri=1  uid=2  gid=3
```

**A transfer names the track by gid, not by uri.** `ContextTrack` has a uri
field and a real transfer left it empty, carrying uid and gid only. Reading
field 1 alone finds nothing and reports there is nothing to play -- having
just claimed the device, which is the worst of both. The gid is the same 16
bytes everything else here base62-encodes, and
`spotifygtk_gid_to_base62()` already exists.

Checked against the real payload from a phone: position 40689 ms, not paused,
gid encoding to `6XmLwx2FpuY4k2u77Uai7C` -- which is the track the transfer's
own context uri names, so the decode agrees with itself from two directions.

`session.c` emits `transfer-requested (uri, position_ms, paused)` and the
window starts playback, seeks and pauses. The protocol layer owns no player,
and the transfer arrives on a socket callback nowhere the UI could reach.

Verified without waiting for a phone: `SPOTIFY_CLUSTER_SELFTEST=1` builds a
ClusterUpdate naming this device, wraps it as the dealer does, and pushes it
through the same entry point a real message takes.

That self-test earned its keep immediately. It printed
`active=86dc1e0` -- seven characters where a device id is forty. Hoisting
`static gchar device_id[41]` to file scope had turned the local into a
`gchar *`, so `sizeof device_id` became 8 and `g_strlcpy` copied seven hex
digits. The client had been **registering itself as `86dc1e0`**, and the
server accepted it and echoed it back, so "is our device in the cluster?"
still answered yes. A real transfer would have been addressed to an id we no
longer used.

## Two other things that log showed

- **Playback recovers from CDN stalls.** Twice, `no response for logical offset
  ... after 25s`, re-resolved, retried, and carried on. That path works under
  real conditions.
- **The AP link dropped mid-session and nothing noticed.**
  `ap.c: receive loop header read failed: Connection reset by peer` at
  23:54:42, with playback continuing from the CDN regardless. Worth knowing,
  because a dead AP link is what "songs randomly do not play" looks like later.

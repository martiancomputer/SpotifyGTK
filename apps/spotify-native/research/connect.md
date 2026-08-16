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

**But the device is not in the cluster that comes back**, by name or by id.
Checked deliberately, because a 200 only says the request was understood. The
cluster returned is almost entirely the *web player's* `PlayerState` — its
playlist context and tracks — which is what was actually playing elsewhere.

Tried and ruled out: a device id that was 40 characters but not hex. A real
40-hex id behaves identically.

### Two more hypotheses, both tried and both wrong

- **A player state.** `Device.player_state` is now sent — an idle one:
  timestamp, position 0, `is_playing` false, `is_paused` false,
  `is_buffering` false, `is_system_initiated` true. The request grew from 121
  to 140 bytes and still answers **200 with the device absent**. So "the server
  will not list a device that has never reported a state" is not the
  explanation, at least not for a state this minimal.
- **A `ClusterUpdate` arriving late.** It does not. The dealer socket stays
  open and logs everything it receives; across ~50 seconds after a successful
  PUT, the only message ever received is the initial connection id. So the
  device is not being merged and announced a moment later either.

- **`client_id`.** Now sent — `NATIVE_AUTH_CLIENT_ID`, Spotify's own keymaster
  id, the one sign-in already uses. Worth being clear that this is *not* a
  developer-portal registration: a portal id is scoped to the Web API, which is
  a different surface from spclient, and requiring one would mean every user
  registering an app before their speakers appeared. The request grew to 176
  bytes; the answer is unchanged. Not the cause.

### What is genuinely untested
- **`member_type`.** `CONNECT_STATE` (2) is the obvious reading;
  `CONNECT_STATE_EXTENDED` (5) exists and its meaning is unknown.
- **`put_state_reason`.** `NEW_DEVICE` (3) is what a first announcement looks
  like, but `NEW_CONNECTION` (9) exists and may be what pairs with having just
  opened a dealer socket.
- **Keeping the socket alive.** The client has `supports_ping_request`, and the
  probe never answers anything. A connection the server considers dead may have
  its device reaped immediately.

The one check not available from here is the only one that is authoritative:
**does "SpotifyGTK" appear in the Connect device picker on another client?**
The PUT response and the dealer are both saying nothing, and they may both be
the wrong place to look.

`SPOTIFY_PROBE_DEALER=1 SPOTIFY_PROBE_PUTSTATE=1` reruns it;
`SPOTIFY_PROBE_CLUSTER=1` additionally dumps what came back.
4. Handle `ClusterUpdate` so remote control actually works.

Steps 1 and 2 are worth doing before any of the encoding: they answer whether
this is possible for a third-party client at all, which is not yet known.

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

## Suggested order, each step provable on its own

1. Extend `apresolve` to return the `dealer` list. Verifiable offline.
2. Open the dealer WebSocket and log the connection id. **This is the gate** —
   if the dealer refuses us, nothing after it matters, and it costs one probe
   to find out.
3. Encode `DeviceInfo` + `Capabilities` and PUT it, then read the cluster back
   and confirm the device appears from another client.
4. Handle `ClusterUpdate` so remote control actually works.

Steps 1 and 2 are worth doing before any of the encoding: they answer whether
this is possible for a third-party client at all, which is not yet known.

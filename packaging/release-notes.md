## Requires Spotify Premium

Playback is licensed per account at the protocol level. A free account is
refused an audio key for **every** format a track offers — Vorbis 320/160/96
and AAC alike — so it cannot play regardless of which client asks. This is not
a limitation of this client, and no setting changes it.

## Which download

- **AppImage** — runs on any distribution with glibc 2.39 or newer, whatever
  its libadwaita version, because the GTK stack is bundled. `chmod +x` and run.
- **.deb** — smaller, links against the system GTK stack. Needs libadwaita
  1.4 or newer: Ubuntu 24.04, Debian 13, or later. **Ubuntu 22.04 will not
  work** — it ships libadwaita 1.1.
- **Windows** — a portable zip is built separately and is not attached here.

## Known limitations

- One track at a time: no gapless or crossfade yet, and each track re-opens
  its own access-point connection.
- No shuffle, repeat, or like. Liking needs a library-write path over Mercury,
  which has never been exercised against a live server.
- Playlists and listening history have no data source and say so in the UI.

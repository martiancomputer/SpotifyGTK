## Requires Spotify Premium

Playback is licensed per account at the protocol level. A free account is
refused an audio key for **every** format a track offers — Vorbis 320/160/96
and AAC alike — so it cannot play regardless of which client asks. This is not
a limitation of this client, and no setting changes it.

## Which download

- **.deb** — links the system GTK stack. Requires **GTK 4.22 and libadwaita
  1.9 or newer**, which the package enforces rather than installing and looking
  wrong. Arch, Fedora 42+, Debian sid and similar. **Ubuntu 24.04 will not
  work** — it ships GTK 4.14.
- **AppImage** — attached when available, and built by hand rather than by CI.
  It bundles the GTK stack, so the machine it is built on decides how it looks;
  built on an older base the layout differs from the screenshots. Reach for it
  where the deb refuses to install.
- **Windows** — a portable zip is built separately and is not attached here.

## Why the version requirement is strict

The UI is laid out against a specific GTK, not merely a minimum one. Widget
metrics changed enough between 4.14 and 4.22 that the same source produces a
visibly different window — cover art stops filling its panel, the playback and
volume bars take other sizes. Rather than thread version-dependent handling
through the UI, the build and the package both refuse rather than look wrong.
`-Dallow_old_gtk=true` overrides it if you would rather have that than nothing.

## Known limitations

- One track at a time: no gapless or crossfade yet, and each track re-opens
  its own access-point connection.
- No shuffle, repeat, or like. Liking needs a library-write path over Mercury,
  which has never been exercised against a live server.
- Playlists and listening history have no data source and say so in the UI.

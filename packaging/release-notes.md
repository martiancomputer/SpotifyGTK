# SpotifyGTK 1.1.8

This update aligns Search results with Liked Songs, keeps the Search album
shelf at the user's chosen scroll position, and places its scrollbar below the
visible album shelf. The Library header now responds to scroll input without
reversing when GTK adjusts the viewport during its animation.

Dark Plus uses a darker main content surface. The minimum window size now keeps
the playback controls and Library sorting controls visible. SpotifyGTK also
remembers the last song and position, reopening paused so playback can resume
from that point.

## Downloads

- `spotifygtk-1.1.8-release-linux-debian-x86-64.deb` — includes the pinned GTK
  runtime; other operating-system libraries are provided by the distribution.
- `spotifygtk-1.1.8-release-linux-arch-x86-64.appimage` — bundles the pinned
  GTK runtime.
- `spotifygtk-1.1.8-release-windows-x86-64.msix` — signed with the established
  SpotifyGTK publisher certificate.
- `spotifygtk-1.1.8-release-windows-x86-64.cer` — public signing certificate;
  install it into **Trusted People** before installing the MSIX.

The app icon remains omitted pending the finalized design. The MSIX contains
transparent placeholders for Windows' required logo files.

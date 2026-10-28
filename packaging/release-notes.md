# SpotifyGTK 1.1.5

This update improves Windows rendering and layout at non-default interface
scales. It fixes focus changes that could shift oversized content across the
viewport, fractional-scale edges in translucent headers, and pane breakpoints
that did not account for the selected scale. Windows packages now include the
C++ runtime needed by Cairo and bundled codecs, and use the same font rendering
settings as Linux.

## Downloads

- `spotifygtk-1.1.5-release-linux-debian-x86-64.deb` — includes the pinned GTK
  runtime; other operating-system libraries are provided by the distribution.
- `spotifygtk-1.1.5-release-linux-arch-x86-64.appimage` — bundles the pinned
  GTK runtime.
- `spotifygtk-1.1.5-release-windows-x86-64.msix` — signed with the established
  SpotifyGTK publisher certificate.
- `spotifygtk-1.1.5-release-windows-x86-64.cer` — public signing certificate;
  install it into **Trusted People** before installing the MSIX.

The app icon remains omitted pending the finalized design. The MSIX contains
transparent placeholders for Windows' required logo files.

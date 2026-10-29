# SpotifyGTK 1.1.6

This update removes the GTK scroller frame and undershoot line at translucent
header edges. The theme-painted edge could leave a dark one-pixel seam where
the glass fade meets scrolling content, especially at fractional interface
scales. The change applies to Search, Liked Songs and artist pages.

Linux and Windows CI builds passed. The Windows build compiles with the pinned
GTK runtime; please report whether the seam is gone in your Windows setup.

## Downloads

- `spotifygtk-1.1.6-release-linux-debian-x86-64.deb` — includes the pinned GTK
  runtime; other operating-system libraries are provided by the distribution.
- `spotifygtk-1.1.6-release-linux-arch-x86-64.appimage` — bundles the pinned
  GTK runtime.
- `spotifygtk-1.1.6-release-windows-x86-64.msix` — signed with the established
  SpotifyGTK publisher certificate.
- `spotifygtk-1.1.6-release-windows-x86-64.cer` — public signing certificate;
  install it into **Trusted People** before installing the MSIX.

The app icon remains omitted pending the finalized design. The MSIX contains
transparent placeholders for Windows' required logo files.

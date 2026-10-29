# SpotifyGTK 1.1.7

This update fixes the remaining thin line at translucent header edges. It
aligns the textured banner and glass backdrop at the same device-pixel edge,
including at fractional interface scales. The fix applies to Search,
Liked Songs and artist pages without changing their layout or scroll position.

The textured Vulkan regression passed from 75% through 130% scale in light and
dark themes, including a 2x display. Linux and Windows CI builds passed. The
line was also confirmed gone in the running Linux app.

## Downloads

- `spotifygtk-1.1.7-release-linux-debian-x86-64.deb` — includes the pinned GTK
  runtime; other operating-system libraries are provided by the distribution.
- `spotifygtk-1.1.7-release-linux-arch-x86-64.appimage` — bundles the pinned
  GTK runtime.
- `spotifygtk-1.1.7-release-windows-x86-64.msix` — signed with the established
  SpotifyGTK publisher certificate.
- `spotifygtk-1.1.7-release-windows-x86-64.cer` — public signing certificate;
  install it into **Trusted People** before installing the MSIX.

The app icon remains omitted pending the finalized design. The MSIX contains
transparent placeholders for Windows' required logo files.

# SpotifyGTK 1.1.3

SpotifyGTK 1.1.3 ships Linux and Windows builds compiled against the repository's
pinned GTK4-Enhanced source. The Debian package and Windows MSIX carry that GTK
runtime, so their widget layout does not depend on a system GTK release.

## Downloads

- `spotifygtk-1.1.3-release-linux-debian-x86-64.deb` — includes the pinned
  GTK runtime; other operating-system libraries are provided by the distro.
- `spotifygtk-1.1.3-release-linux-arch-x86-64.appimage` — bundles the pinned
  GTK runtime and runs without depending on a system GTK version.
- `spotifygtk-1.1.3-release-windows-x86-64.msix` — signed with the established
  SpotifyGTK publisher certificate.
- `spotifygtk-1.1.3-release-windows-x86-64.cer` — public certificate to
  install into **Trusted People** before installing the MSIX; see
  `INSTALL-MSIX.md`.

The app icon is intentionally omitted pending the finalized design. MSIX's
required logo slots are transparent placeholders; GTK's interface icon theme
is included so symbolic controls still render.

## What's included

- Fixed Windows 11 edge snapping and fullscreen sizing, matched Windows font
  rendering to the bundled interface fonts, and preserved responsive resizing.
- Release builds require FFmpeg local-file decoding and verify that the
  AppImage uses the exact pinned GTK runtime.

- Pinned GTK4-Enhanced with selectable 30, 60, 90, 120, 144, 240 and 360 FPS
  ceilings where the experimental frame clock is enabled.
- Optional unsynchronized Wayland/Vulkan rendering for testing refresh rates
  above the display's normal presentation cap.
- A searchable keyboard-shortcut editor with playback, navigation, panel,
  library, and interface bindings.
- Searchable artist names in Now Playing open the selected artist's page.
- Liked Songs keeps nearby cover art loading during fast scrolling, and the
  seek bar hides its circular thumb while preserving drag-to-seek behavior.
- Interface scaling presets and custom scale from 25% to 300%, plus keyboard
  shortcuts for scaling and play/pause.
- Persistent volume, Linux-matching bundled Windows fonts, responsive snapped
  windows, and fullscreen controls.
- Home, Search, Library, Liked Songs, album and artist pages, Spotify Connect,
  local files, and the existing audio settings.

Spotify playback requires a Premium account. SpotifyGTK does not bypass account
or service access checks.

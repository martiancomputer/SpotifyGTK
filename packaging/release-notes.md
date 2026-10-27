# SpotifyGTK 1.1.4

This release starts loading Liked Songs and saved albums as soon as sign-in is
ready, instead of waiting for the first visit to either page. Saved album names,
artwork IDs and date-added order are restored from an account-scoped disk cache
while the server refreshes in the background.

The pinned GTK runtime now uses consistent row-height estimates and preserves the
old item position when a provisional list is replaced by the full collection.
This fixes the Library and Liked Songs loading jump without forcing a later
scroll. Same-order album metadata refreshes update existing cards in place.

Search, Liked Songs and artist pages retain their theme-aware translucent
headers. The artist release list keeps separate headings and tracks, and the
visible row pool stays bounded for large discographies.

## Downloads

- `spotifygtk-1.1.4-release-linux-debian-x86-64.deb` — includes the pinned GTK
  runtime; other operating-system libraries are provided by the distribution.
- `spotifygtk-1.1.4-release-linux-arch-x86-64.appimage` — bundles the pinned
  GTK runtime.
- `spotifygtk-1.1.4-release-windows-x86-64.msix` — signed with the established
  SpotifyGTK publisher certificate.
- `spotifygtk-1.1.4-release-windows-x86-64.cer` — public signing certificate;
  install it into **Trusted People** before installing the MSIX.

The app icon remains omitted pending the finalized design. The MSIX contains
transparent placeholders for Windows' required logo files.

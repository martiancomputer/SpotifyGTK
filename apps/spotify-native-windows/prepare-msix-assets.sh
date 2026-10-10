#!/usr/bin/env bash
set -euo pipefail
asset_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
asset_output="${1:-$asset_root/build/msix-assets}"
command -v rsvg-convert >/dev/null || {
  echo 'rsvg-convert (librsvg) is required to render the existing app icon.' >&2
  exit 1
}
mkdir -p "$asset_output"
for asset in 'StoreLogo:50' 'Square44x44Logo:44' 'Square150x150Logo:150' \
             'Square44x44Logo.scale-200:88' 'Square150x150Logo.scale-200:300'; do
  asset_name="${asset%:*}"
  asset_size="${asset#*:}"
  rsvg-convert -w "$asset_size" -h "$asset_size" \
    "$asset_root/../spotify-native/data/com.github.spotifygtk.SpotifyNative.svg" \
    -o "$asset_output/$asset_name.png"
done

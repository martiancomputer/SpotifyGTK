#!/usr/bin/env bash
#
# build-deb.sh — build a .deb of spotify-native.
#
# A deb links against the system's GTK stack rather than bundling it, so it is
# small (a few hundred KB) but only installs where those libraries are new
# enough. libadwaita >= 1.4 is the binding constraint, which is Ubuntu 24.04,
# Debian 13 or later. Ubuntu 22.04 ships 1.1 and is not a target -- see the
# README. For distributions older than that, the AppImage is the answer.
#
#   ./packaging/build-deb.sh            # builds into packaging/out/
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
APP="$ROOT/apps/spotify-native"
OUT="$HERE/out"
STAGE="$OUT/deb-root"

VERSION="$(sed -n "s/^ *version: *'\\([^']*\\)'.*/\\1/p" "$APP/meson.build" | head -1)"
ARCH="$(dpkg --print-architecture)"
PKG="spotifygtk_${VERSION}_${ARCH}"

echo "==> building spotify-native $VERSION ($ARCH)"

rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN"

# Configure fresh so the staged tree matches a release build rather than
# whatever the working build directory happens to hold.
BUILD="$OUT/build-deb"
rm -rf "$BUILD"
meson setup "$BUILD" "$APP" \
  --native-file "$APP/build-profiles/stable.ini" \
  --prefix /usr --buildtype release >/dev/null
meson compile -C "$BUILD" >/dev/null
DESTDIR="$STAGE" meson install -C "$BUILD" --quiet >/dev/null

# Runtime dependencies, resolved from what the binary actually links rather
# than from a hand-written list that drifts. shlibdeps needs the control file
# to already exist, hence the two-step.
cat > "$STAGE/DEBIAN/control" <<EOF
Package: spotifygtk
Version: $VERSION
Section: sound
Priority: optional
Architecture: $ARCH
Maintainer: martiancomputer <noreply@github.com>
Homepage: https://github.com/martiancomputer/SpotifyGTK
Description: Native Spotify client written in C
 A Spotify client that speaks Spotify's own protocol directly and decodes and
 plays audio itself, rather than driving another running client. Written
 entirely in C, with no Rust and no Electron.
 .
 Includes a 15-band graphic equaliser and an optional resampler.
 .
 Requires a Spotify Premium account: playback is licensed per account at the
 protocol level, so a free account cannot play regardless of client.
EOF

if command -v dpkg-shlibdeps >/dev/null; then
  ( cd "$STAGE" && dpkg-shlibdeps -O "usr/bin/spotify-native" 2>/dev/null ) \
    | sed 's/^shlibs:Depends=/Depends: /' >> "$STAGE/DEBIAN/control" || {
      echo "    dpkg-shlibdeps failed; falling back to a declared list" >&2
      echo "Depends: libgtk-4-1, libadwaita-1-0 (>= 1.4), libsoup-3.0-0, libjson-glib-1.0-0, libogg0, libvorbis0a, libvorbisfile3, libssl3 | libssl3t64" >> "$STAGE/DEBIAN/control"
    }
else
  echo "Depends: libgtk-4-1, libadwaita-1-0 (>= 1.4), libsoup-3.0-0, libjson-glib-1.0-0, libogg0, libvorbis0a, libvorbisfile3" >> "$STAGE/DEBIAN/control"
fi

# Trigger the caches a desktop needs refreshed; without these the launcher
# entry and icon do not appear until something else happens to rebuild them.
cat > "$STAGE/DEBIAN/triggers" <<'EOF'
activate-noawait update-desktop-database
activate-noawait /usr/share/icons/hicolor
EOF

find "$STAGE" -type d -exec chmod 755 {} +
find "$STAGE/usr" -type f -exec chmod 644 {} + 2>/dev/null || true
chmod 755 "$STAGE"/usr/bin/* 2>/dev/null || true

dpkg-deb --build --root-owner-group "$STAGE" "$OUT/$PKG.deb" >/dev/null
echo "==> $OUT/$PKG.deb"
dpkg-deb --info "$OUT/$PKG.deb" | sed -n '/Package:/,/^$/p' | head -12
echo
echo "contents:"
dpkg-deb --contents "$OUT/$PKG.deb" | awk '{print "  "$6}' | grep -v '/$'

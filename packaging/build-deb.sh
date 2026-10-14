#!/usr/bin/env bash
#
# build-deb.sh — build a .deb of spotify-native.
#
# The package carries the pinned GTK fork so GTK version differences on the
# target distro cannot change the app layout. Other OS libraries remain distro
# dependencies. Build the fork first with packaging/build-enhanced-gtk.sh.
#
#   ./packaging/build-deb.sh            # builds into packaging/out/
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
APP="$ROOT/apps/spotify-native"
OUT="$HERE/out"
STAGE="$OUT/deb-root"
GTK_ENHANCED_PREFIX="${GTK_ENHANCED_PREFIX:-$ROOT/_stage/gtk4-enhanced/usr}"

[[ -s "$GTK_ENHANCED_PREFIX/lib/pkgconfig/gtk4.pc" ]] || {
  echo 'Build the pinned GTK fork first with packaging/build-enhanced-gtk.sh.' >&2
  exit 1
}
export PKG_CONFIG_PATH="$GTK_ENHANCED_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$GTK_ENHANCED_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
test "$(pkg-config --variable=libdir gtk4)" = "$GTK_ENHANCED_PREFIX/lib" || {
  echo 'Release build did not resolve GTK from the pinned fork.' >&2
  exit 1
}

VERSION="$(sed -n "s/^ *version: *'\\([^']*\\)'.*/\\1/p" "$APP/meson.build" | head -1)"
ARCH="$(dpkg --print-architecture)"
PKG="spotifygtk-${VERSION}-release-linux-debian-x86-64"

echo "==> building spotify-native $VERSION ($ARCH)"

rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN"

# Configure fresh so the staged tree matches a release build rather than
# whatever the working build directory happens to hold.
BUILD="$OUT/build-deb"
rm -rf "$BUILD"
meson setup "$BUILD" "$APP" \
  --native-file "$APP/build-profiles/stable.ini" \
  --prefix /usr --buildtype release -Dtests=false \
  -Dlocal_playback=enabled \
  -Dallow_old_libadwaita=true \
  "-Dc_link_args=-Wl,-rpath,/usr/lib/spotifygtk/gtk" >/dev/null
meson compile -C "$BUILD" >/dev/null
DESTDIR="$STAGE" meson install -C "$BUILD" --quiet >/dev/null

# Carry GTK plus the complete shared-library runtime directory. GTK's lower-
# level dependencies (GLib, Pango, Cairo, etc.) may come from pinned Meson wraps
# on older builders, so the package must not bind to a mismatched system ABI.
mkdir -p "$STAGE/usr/lib/spotifygtk/gtk"
cp -a "$GTK_ENHANCED_PREFIX/lib"/lib*.so* "$STAGE/usr/lib/spotifygtk/gtk/"
command -v patchelf >/dev/null || { echo 'patchelf is required to make bundled libraries relocatable.' >&2; exit 1; }
while IFS= read -r -d '' library; do
  patchelf --set-rpath '$ORIGIN' "$library"
done < <(find "$STAGE/usr/lib/spotifygtk/gtk" -type f -name 'lib*.so*' -print0)
patchelf --set-rpath '$ORIGIN/../lib/spotifygtk/gtk' "$STAGE/usr/bin/spotify-native"
patchelf --set-rpath '$ORIGIN/../lib/spotifygtk/gtk' "$STAGE/usr/bin/spotify-native-harness"
gtk_license_dir="$STAGE/usr/share/licenses/GTK4-Enhanced"
mkdir -p "$gtk_license_dir"
for notice in COPYING AUTHORS UPSTREAM.md FRAME_POLICY.md; do
  [[ -s "$ROOT/dependencies/gtk4-enhanced/$notice" ]] || {
    echo "Missing pinned GTK notice: $notice" >&2; exit 1;
  }
  cp "$ROOT/dependencies/gtk4-enhanced/$notice" "$gtk_license_dir/"
done
printf 'Corresponding source: https://github.com/martiancomputer/GTK4-Enhanced\nRevision: %s\n' \
  "$(git -C "$ROOT/dependencies/gtk4-enhanced" rev-parse HEAD)" > "$gtk_license_dir/SOURCE.txt"
wrapped_sources="$ROOT/_build/gtk4-enhanced/subprojects"
if [[ -d "$wrapped_sources" ]]; then
  while IFS= read -r -d '' notice; do
    relative="${notice#"$wrapped_sources"/}"
    destination="$gtk_license_dir/Dependencies/${relative%/*}"
    mkdir -p "$destination"
    cp "$notice" "$destination/"
  done < <(find "$wrapped_sources" -mindepth 2 -maxdepth 3 -type f \
    \( -iname COPYING -o -iname LICENSE -o -iname 'LICENSE.*' \) -print0)
fi

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
  ( cd "$STAGE" && dpkg-shlibdeps -l"$STAGE/usr/lib/spotifygtk/gtk" -O \
      usr/bin/spotify-native usr/bin/spotify-native-harness 2>/dev/null ) \
    | sed 's/^shlibs:Depends=/Depends: glib-networking, /' >> "$STAGE/DEBIAN/control" || {
      echo "    dpkg-shlibdeps failed; falling back to a declared list" >&2
      echo "Depends: glib-networking, libadwaita-1-0, libsoup-3.0-0, libjson-glib-1.0-0, libogg0, libvorbis0a, libvorbisfile3, libssl3 | libssl3t64" >> "$STAGE/DEBIAN/control"
    }
else
  echo "Depends: glib-networking, libadwaita-1-0, libsoup-3.0-0, libjson-glib-1.0-0, libogg0, libvorbis0a, libvorbisfile3" >> "$STAGE/DEBIAN/control"
fi

# Avoid using the system app icon until the finalized design is ready. GTK's
# symbolic theme assets remain bundled separately in portable formats.
sed -i '/^Icon=/d' "$STAGE/usr/share/applications/"*.desktop
rm -f "$STAGE/usr/share/icons/hicolor/scalable/apps/com.github.spotifygtk.SpotifyNative.svg"

# Trigger the caches a desktop needs refreshed; without these the launcher
# entry and icon do not appear until something else happens to rebuild them.
cat > "$STAGE/DEBIAN/triggers" <<'EOF'
activate-noawait update-desktop-database
activate-noawait /usr/share/icons/hicolor
EOF

find "$STAGE" -type d -exec chmod 755 {} +
find "$STAGE/usr" -type f -exec chmod 644 {} + 2>/dev/null || true
find "$STAGE/usr/lib/spotifygtk/gtk" -type f -name 'libgtk-4.so*' -exec chmod 755 {} +
chmod 755 "$STAGE"/usr/bin/* 2>/dev/null || true

dpkg-deb --build --root-owner-group "$STAGE" "$OUT/$PKG.deb" >/dev/null
echo "==> $OUT/$PKG.deb"
dpkg-deb --info "$OUT/$PKG.deb" | sed -n '/Package:/,/^$/p' | head -12
echo
echo "contents:"
dpkg-deb --contents "$OUT/$PKG.deb" | awk '{print "  "$6}' | grep -v '/$'

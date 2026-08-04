#!/usr/bin/env bash
#
# build-appimage.sh — build an AppImage of spotify-native.
#
# Why this exists alongside the deb: a deb links against the system's GTK stack,
# so it only installs where libadwaita is already >= 1.4. An AppImage bundles
# that stack, so it runs on any distribution whose glibc is new enough --
# including ones whose libadwaita is far too old for the deb.
#
# That is also its limit. An AppImage cannot bundle glibc, so the build host's
# glibc becomes the floor. Built on Ubuntu 24.04 (glibc 2.39) because
# libadwaita >= 1.4 is required to compile at all, which rules out older bases.
#
#   ./packaging/build-appimage.sh       # builds into packaging/out/
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
APP="$ROOT/apps/spotify-native"
OUT="$HERE/out"
APPDIR="$OUT/AppDir"
TOOLS="$OUT/tools"

VERSION="$(sed -n "s/^ *version: *'\\([^']*\\)'.*/\\1/p" "$APP/meson.build" | head -1)"
ID="com.github.spotifygtk.SpotifyNative"

echo "==> building spotify-native $VERSION AppImage"

rm -rf "$APPDIR"
mkdir -p "$APPDIR" "$TOOLS" "$OUT"

BUILD="$OUT/build-appimage"
rm -rf "$BUILD"
meson setup "$BUILD" "$APP" \
  --native-file "$APP/build-profiles/stable.ini" \
  --prefix /usr --buildtype release >/dev/null
meson compile -C "$BUILD" >/dev/null
DESTDIR="$APPDIR" meson install -C "$BUILD" --quiet >/dev/null

# linuxdeploy and its GTK plugin. The plugin is what collects the pieces a GTK
# app needs beyond its own libraries -- gdk-pixbuf loaders, GIO modules
# (glib-networking, without which every HTTPS request fails), compiled
# GSettings schemas and the icon theme. Missing any of those produces an
# AppImage that starts and then fails in a way that looks unrelated to
# packaging; the Windows bundle hit exactly that with TLS.
fetch () {  # fetch <url> <dest>
  if [ ! -x "$2" ]; then
    echo "    fetching $(basename "$2")"
    curl -fsSL "$1" -o "$2"
    chmod +x "$2"
  fi
}
BASE=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous
fetch "$BASE/linuxdeploy-x86_64.AppImage" "$TOOLS/linuxdeploy"
fetch "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh" \
      "$TOOLS/linuxdeploy-plugin-gtk.sh"

# CI runners have no FUSE, so the tools must unpack themselves rather than
# mount. Harmless elsewhere.
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$TOOLS:$PATH"
export OUTPUT="$OUT/SpotifyGTK-${VERSION}-x86_64.AppImage"

# DEPLOY_GTK_VERSION tells the plugin which stack to collect; without it the
# plugin guesses and can bundle GTK3 alongside, which bloats the image and can
# load the wrong theme engine at runtime.
DEPLOY_GTK_VERSION=4 "$TOOLS/linuxdeploy" \
  --appdir "$APPDIR" \
  --plugin gtk \
  --desktop-file "$APPDIR/usr/share/applications/$ID.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/$ID.svg" \
  --output appimage

echo "==> $OUTPUT"
ls -lh "$OUTPUT" | awk '{print "    "$5}'

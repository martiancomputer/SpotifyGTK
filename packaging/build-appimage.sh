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
# GIO modules, which the GTK plugin does not collect.
#
# glib-networking provides GIO's TLS backend, and without it every HTTPS
# request fails with "TLS support is not available" -- which reads as a network
# fault and is not one. The app still signs in, because the access point is raw
# TCP, and then cannot fetch a client-token, so the session dies one step later.
# The Windows bundle shipped with exactly this bug and so did the first
# AppImage; both looked fine until something tried to talk to an HTTPS endpoint.
GIO_SRC="$(pkg-config --variable=giomoduledir gio-2.0 2>/dev/null || true)"
[ -d "$GIO_SRC" ] || GIO_SRC="/usr/lib/$(uname -m)-linux-gnu/gio/modules"
if [ -d "$GIO_SRC" ] && ls "$GIO_SRC"/libgio*.so >/dev/null 2>&1; then
  mkdir -p "$APPDIR/usr/lib/gio/modules"
  cp -v "$GIO_SRC"/libgio*.so "$APPDIR/usr/lib/gio/modules/" | sed 's/^/    /'
else
  echo "!!! no GIO modules found in $GIO_SRC -- the AppImage will have no TLS" >&2
  echo "    install glib-networking and rebuild" >&2
  exit 1
fi

# linuxdeploy sources every script in apprun-hooks/ from the AppRun it
# generates, which is how the bundled modules get found at runtime rather than
# the host's -- there may be none on the host at all.
mkdir -p "$APPDIR/apprun-hooks"
cat > "$APPDIR/apprun-hooks/gio-modules.sh" <<'HOOK'
export GIO_MODULE_DIR="$APPDIR/usr/lib/gio/modules"
HOOK

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

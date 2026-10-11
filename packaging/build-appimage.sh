#!/usr/bin/env bash
#
# build-appimage.sh — build an AppImage of spotify-native.
#
# The release AppImage links against and bundles the repository's pinned GTK
# fork, so the build host's GTK package version cannot change its widget sizes.
# The rest of the runtime dependency closure is collected by linuxdeploy.
#
# Two limits follow from bundling, and they pull against each other.
#
# It cannot bundle glibc, so the runner's glibc remains the minimum supported
# runtime. GTK itself comes from dependencies/gtk4-enhanced and is checked
# against that submodule before the artifact is built.
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
GTK_ENHANCED_PREFIX="${GTK_ENHANCED_PREFIX:-$ROOT/_stage/gtk4-enhanced/usr}"

if [[ ! -s "$GTK_ENHANCED_PREFIX/lib/pkgconfig/gtk4.pc" ]]; then
  echo 'Build the pinned GTK fork first with packaging/build-enhanced-gtk.sh.' >&2
  exit 1
fi
export PKG_CONFIG_PATH="$GTK_ENHANCED_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$GTK_ENHANCED_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
test "$(pkg-config --variable=libdir gtk4)" = "$GTK_ENHANCED_PREFIX/lib" || {
  echo 'Release build did not resolve GTK from the pinned fork.' >&2
  exit 1
}

VERSION="$(sed -n "s/^ *version: *'\\([^']*\\)'.*/\\1/p" "$APP/meson.build" | head -1)"
ID="com.github.spotifygtk.SpotifyNative"

echo "==> building spotify-native $VERSION AppImage"

rm -rf "$APPDIR"
mkdir -p "$APPDIR" "$TOOLS" "$OUT"

BUILD="$OUT/build-appimage"
rm -rf "$BUILD"
meson setup "$BUILD" "$APP" \
  --native-file "$APP/build-profiles/stable.ini" \
  --prefix /usr --buildtype release -Dtests=false \
  -Dallow_old_libadwaita=true \
  "-Dc_link_args=-Wl,-rpath,$GTK_ENHANCED_PREFIX/lib" >/dev/null
meson compile -C "$BUILD" >/dev/null
gtk_source_version="$(sed -n "s/.*version: '\([^']*\)'.*/\1/p" "$ROOT/dependencies/gtk4-enhanced/meson.build" | head -1)"
test "$(pkg-config --modversion gtk4)" = "$gtk_source_version" || {
  echo 'Release build GTK version does not match the staged fork.' >&2
  exit 1
}
DESTDIR="$APPDIR" meson install -C "$BUILD" --quiet >/dev/null

# The fork's shared GTK library is redistributed in this image, so retain the
# exact upstream notices and corresponding source revision beside the app.
gtk_license_dir="$APPDIR/usr/share/licenses/GTK4-Enhanced"
mkdir -p "$gtk_license_dir"
for notice in COPYING AUTHORS UPSTREAM.md FRAME_POLICY.md; do
  [[ -s "$ROOT/dependencies/gtk4-enhanced/$notice" ]] || {
    echo "Missing pinned GTK notice: $notice" >&2; exit 1;
  }
  cp "$ROOT/dependencies/gtk4-enhanced/$notice" "$gtk_license_dir/"
done
printf 'Corresponding source: https://github.com/martiancomputer/GTK4-Enhanced\nRevision: %s\n' \
  "$(git -C "$ROOT/dependencies/gtk4-enhanced" rev-parse HEAD)" > "$gtk_license_dir/SOURCE.txt"

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
if [ -z "$GIO_SRC" ] || ! compgen -G "$GIO_SRC/libgio*.so" >/dev/null; then
  GIO_SRC="/usr/lib/$(uname -m)-linux-gnu/gio/modules"
fi
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

# This GTK fork does not install GTK loadable modules. Keep the conventional
# module directory present for linuxdeploy-plugin-gtk, and rewrite its GTK4
# module search path to the AppDir (the plugin assumes a system /usr prefix).
mkdir -p "$GTK_ENHANCED_PREFIX/lib/gtk-4.0"
python3 - "$TOOLS/linuxdeploy-plugin-gtk.sh" <<'PY'
from pathlib import Path
import sys

plugin = Path(sys.argv[1])
text = plugin.read_text()
source = 'export GTK_PATH="\\$APPDIR/${gtk4_path/$LD_GTK_LIBRARY_PATH//usr/lib}"'
target = 'export GTK_PATH="\\$APPDIR/usr/lib/gtk-4.0"'
if source not in text:
    raise SystemExit('Pinned GTK AppImage path patch no longer matches linuxdeploy plugin.')
plugin.write_text(text.replace(source, target, 1))
PY

# CI runners have no FUSE, so the tools must unpack themselves rather than
# mount. Harmless elsewhere.
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$TOOLS:$PATH"
export OUTPUT="$OUT/spotifygtk-${VERSION}-release-linux-arch-x86-64.appimage"

# DEPLOY_GTK_VERSION tells the plugin which stack to collect; without it the
# plugin guesses and can bundle GTK3 alongside, which bloats the image and can
# load the wrong theme engine at runtime.
# Keep GTK's own icon theme (used by controls), but do not package the
# unfinished SpotifyGTK application icon. linuxdeploy can produce an AppImage
# without an --icon-file; desktop shells will use their generic fallback.
rm -f "$APPDIR/usr/share/icons/hicolor/scalable/apps/$ID.svg"
sed -i '/^Icon=/d' "$APPDIR/usr/share/applications/$ID.desktop"
DEPLOY_GTK_VERSION=4 "$TOOLS/linuxdeploy" \
  --appdir "$APPDIR" \
  --plugin gtk \
  --desktop-file "$APPDIR/usr/share/applications/$ID.desktop" \
  --output appimage

echo "==> $OUTPUT"
ls -lh "$OUTPUT" | awk '{print "    "$5}'

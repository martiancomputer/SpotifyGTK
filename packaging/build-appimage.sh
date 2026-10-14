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
  -Dlocal_playback=enabled \
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
if source not in text and target not in text:
    raise SystemExit('Pinned GTK AppImage path patch no longer matches linuxdeploy plugin.')
text = text.replace(source, target, 1)

start_marker = 'echo "Installing GDK PixBufs"\n'
end_marker = 'sed -i "s|$gdk_pixbuf_moduledir/||g" "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"\n'
start = text.find(start_marker)
end = text.find(end_marker, start)
if start < 0 or end < 0:
    raise SystemExit('GDK Pixbuf AppImage section no longer matches linuxdeploy plugin.')
end += len(end_marker)
replacement = '''LD_GTK_LIBRARY_PATH="/usr/lib"
echo "Installing GDK PixBufs"
gdk_libdir="$(get_pkgconf_variable "libdir" "gdk-pixbuf-2.0" "$LD_GTK_LIBRARY_PATH")"
gdk_pixbuf_binarydir="$(get_pkgconf_variable "gdk_pixbuf_binarydir" "gdk-pixbuf-2.0" "$gdk_libdir/gdk-pixbuf-2.0/2.10.0")"
if [ ! -d "$gdk_pixbuf_binarydir" ]; then
    gdk_pixbuf_binarydir="$(find /usr/lib -type d -path '*/gdk-pixbuf-2.0/2.10.0' -print -quit)"
fi
if [ -n "$gdk_pixbuf_binarydir" ] && [ -d "$gdk_pixbuf_binarydir" ]; then
    gdk_pixbuf_cache_file="$gdk_pixbuf_binarydir/loaders.cache"
    gdk_pixbuf_moduledir="$gdk_pixbuf_binarydir/loaders"
    gdk_pixbuf_query="$(search_tool "gdk-pixbuf-query-loaders" "gdk-pixbuf-2.0")"
    copy_lib_tree "$gdk_pixbuf_binarydir" "$APPDIR/"
    cat >> "$HOOKFILE" <<EOF
export GDK_PIXBUF_MODULE_FILE="\\$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"
EOF
    if [ -x "$gdk_pixbuf_query" ]; then
        echo "Updating pixbuf cache in $APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"
        "$gdk_pixbuf_query" > "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"
    else
        echo "WARNING: gdk-pixbuf-query-loaders not found"
    fi
    if [ ! -f "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}" ]; then
        echo "WARNING: loaders.cache file is missing"
    fi
    sed -i "s|$gdk_pixbuf_moduledir/||g" "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"
else
    echo "No legacy GDK Pixbuf modules found; relying on the system GDK Pixbuf backend."
fi
'''
if replacement not in text:
    text = text[:start] + replacement + text[end:]
plugin.write_text(text)
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
# unfinished SpotifyGTK application icon. AppImage launchers still require an
# Icon key, so use the standard generic executable icon until the final art is
# ready.
rm -f "$APPDIR/usr/share/icons/hicolor/scalable/apps/$ID.svg"
sed -i 's/^Icon=.*/Icon=application-x-executable/' \
  "$APPDIR/usr/share/applications/$ID.desktop"
generic_icon="$(find /usr/share/icons -type f \
  -path '*/mimetypes/application-x-executable.png' -print -quit 2>/dev/null)"
if [[ -z "$generic_icon" ]]; then
  echo 'error: the generic Adwaita executable icon is unavailable' >&2
  exit 1
fi
install -D "$generic_icon" \
  "$APPDIR/usr/share/icons/hicolor/256x256/apps/application-x-executable.png"
DEPLOY_GTK_VERSION=4 "$TOOLS/linuxdeploy" \
  --appdir "$APPDIR" \
  --plugin gtk \
  --desktop-file "$APPDIR/usr/share/applications/$ID.desktop"

# The plugin imports host GTK settings, including a forced X11 backend and
# theme. Neither override is appropriate for this GTK4/libadwaita application:
# preserve native display selection and the user's app/desktop theme instead.
sed -i '/^export GDK_BACKEND=/d; /^export GTK_THEME=/d' \
  "$APPDIR/apprun-hooks/linuxdeploy-plugin-gtk.sh"

# Resolve staged libraries explicitly, not merely by SONAME through ldd: a
# plugin's dependency scan can otherwise copy a system library of the same
# name. Include wrapped lower-level dependencies when the fork stages them.
pinned_libraries=()
cp -a "$GTK_ENHANCED_PREFIX/lib"/lib*.so* "$APPDIR/usr/lib/"
while IFS= read -r -d '' library; do
  pinned_libraries+=(--library "$library")
done < <(find "$GTK_ENHANCED_PREFIX/lib" -maxdepth 1 -type f -name 'lib*.so*' -print0)
"$TOOLS/linuxdeploy" --appdir "$APPDIR" "${pinned_libraries[@]}"

# linuxdeploy may strip or patch ELF files, but preserves the build ID. Check
# provenance and the loader's actual resolution before emitting a release.
gtk_build_id () { readelf -n "$1" | sed -n 's/.*Build ID: //p'; }
staged_id="$(gtk_build_id "$GTK_ENHANCED_PREFIX/lib/libgtk-4.so.1")"
bundled_id="$(gtk_build_id "$APPDIR/usr/lib/libgtk-4.so.1")"
[[ -n "$staged_id" && "$staged_id" = "$bundled_id" ]] || {
  echo 'AppImage GTK build ID does not match the pinned staged library.' >&2; exit 1;
}
strings "$APPDIR/usr/lib/libgtk-4.so.1" | grep -Fx 'gtk4-enhanced-frame-policy-version' >/dev/null || {
  echo 'AppImage contains GTK without the enhanced frame policy.' >&2; exit 1;
}
resolved_gtk="$(LD_LIBRARY_PATH="$APPDIR/usr/lib" ldd "$APPDIR/usr/bin/spotify-native" |
  awk '/libgtk-4.so.1 =>/ {print $3}')"
[[ "$(realpath "$resolved_gtk")" = "$(realpath "$APPDIR/usr/lib/libgtk-4.so.1")" ]] || {
  echo 'AppImage runtime resolves GTK outside its bundle.' >&2; exit 1;
}
for component in avformat avcodec avutil swresample; do
  compgen -G "$APPDIR/usr/lib/lib${component}.so.*" >/dev/null || {
    echo "AppImage is missing the FFmpeg $component runtime." >&2; exit 1;
  }
done
"$TOOLS/linuxdeploy" --appdir "$APPDIR" --output appimage
[[ "$(gtk_build_id "$APPDIR/usr/lib/libgtk-4.so.1")" = "$staged_id" ]] || {
  echo 'Final AppImage deployment replaced the pinned GTK library.' >&2; exit 1;
}

echo "==> $OUTPUT"
ls -lh "$OUTPUT" | awk '{print "    "$5}'

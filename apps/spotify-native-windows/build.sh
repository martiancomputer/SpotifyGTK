#!/usr/bin/env bash
#
# build.sh — configure and build ../spotify-native for Windows.
#
# Two modes, picked automatically:
#   - Run under MSYS2/MinGW (native): builds directly, no cross file.
#   - Run on Linux: cross-compiles via cross/mingw-w64-x86_64.ini.
#
# --bundle additionally collects the DLL closure and GTK runtime data into
# build/dist, which is what an installer would package. See README.md for why
# this directory holds no source of its own.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../spotify-native"
BUILD="$HERE/build"
DIST="$BUILD/dist"
CROSS="$HERE/cross/mingw-w64-x86_64.ini"

bundle=false
[[ "${1:-}" == "--bundle" ]] && bundle=true

# MSYSTEM is set by every MSYS2 shell; its absence means we are cross-compiling.
if [[ -n "${MSYSTEM:-}" ]]; then
  echo "==> native MSYS2 build ($MSYSTEM)"
  setup_args=()
else
  echo "==> cross build (linux -> windows)"
  command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 || {
    echo "error: x86_64-w64-mingw32-gcc not found. Install mingw-w64-gcc, or" >&2
    echo "       build under MSYS2 UCRT64 instead (see README.md)." >&2
    exit 1
  }
  setup_args=(--cross-file "$CROSS")
fi

if [[ ! -d "$BUILD" ]]; then
  meson setup "$BUILD" "$SRC" "${setup_args[@]}"
else
  # Reconfiguring keeps an existing build dir usable after option changes.
  meson setup --reconfigure "$BUILD" "$SRC" "${setup_args[@]}"
fi

meson compile -C "$BUILD"
echo "==> built: $BUILD/src/spotify-native.exe"

$bundle || exit 0

# ---------------------------------------------------------------------------
# Bundle: a GTK4 app on Windows needs far more than its own executable.
# ---------------------------------------------------------------------------
echo "==> bundling runtime into $DIST"
mkdir -p "$DIST"
cp "$BUILD/src/spotify-native.exe" "$DIST/" 2>/dev/null || \
  cp "$BUILD/src/spotify-native" "$DIST/spotify-native.exe"

# Resolve the DLL closure. ntldd walks it transitively and is the right tool;
# objdump only lists direct imports, so it needs a manual sweep.
prefix="${MINGW_PREFIX:-/usr/x86_64-w64-mingw32}"
if command -v ntldd >/dev/null 2>&1; then
  ntldd -R "$DIST/spotify-native.exe" \
    | awk '/=>/ {print $3}' \
    | grep -iv 'system32\|windows' \
    | sort -u \
    | while read -r dll; do [[ -f "$dll" ]] && cp -n "$dll" "$DIST/"; done
else
  echo "    ntldd not found -- copy the DLL closure by hand (see README.md)"
fi

# GTK's runtime data. Missing icons are the usual symptom of skipping this:
# the window comes up with every symbolic icon blank.
mkdir -p "$DIST/share/glib-2.0/schemas" "$DIST/share/icons" "$DIST/lib"
cp -rn "$prefix/share/icons/Adwaita"           "$DIST/share/icons/"        2>/dev/null || true
cp -rn "$prefix/share/icons/hicolor"           "$DIST/share/icons/"        2>/dev/null || true
cp -rn "$prefix/share/glib-2.0/schemas/"*.compiled "$DIST/share/glib-2.0/schemas/" 2>/dev/null || true
cp -rn "$prefix/lib/gdk-pixbuf-2.0"            "$DIST/lib/"                2>/dev/null || true

echo "==> $DIST ready (installer packaging is not implemented)"

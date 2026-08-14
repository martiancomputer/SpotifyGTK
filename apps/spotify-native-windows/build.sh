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

# ---------------------------------------------------------------------------
# The DLL closure.
#
# This used to be one pass of `ntldd -R` over the executable, which is wrong in
# a way that only shows up at runtime. GTK loads a good deal of itself with
# dlopen rather than by linking: glib-networking's TLS backend under
# lib/gio/modules, the gdk-pixbuf image loaders. Nothing links them, so no walk
# starting at the .exe can reach them -- and each one drags in dependencies of
# its own that are therefore missing too.
#
# The visible symptom is not a missing file. It is every HTTPS request failing
# with "TLS support is not available", which reads like a network problem and
# sends you looking in the wrong place entirely.
#
# So: copy the loadable modules first, then walk everything in dist repeatedly
# until a pass adds nothing. Direct imports are enough when iterated to a
# fixpoint, which is why objdump is sufficient and works when cross-compiling
# as well as under MSYS2.
# ---------------------------------------------------------------------------
OBJDUMP=""
for c in x86_64-w64-mingw32-objdump objdump; do
  command -v "$c" >/dev/null 2>&1 && { OBJDUMP="$c"; break; }
done
if [[ -z "$OBJDUMP" ]]; then
  echo "error: no objdump found; cannot resolve the DLL closure." >&2
  exit 1
fi

# Loadable modules, before the walk so their own dependencies are picked up.
mkdir -p "$DIST/lib/gio/modules"
cp -n "$prefix/lib/gio/modules/"*.dll "$DIST/lib/gio/modules/" 2>/dev/null || true
cp -rn "$prefix/lib/gdk-pixbuf-2.0"   "$DIST/lib/"             2>/dev/null || true

# A name is ours to ship only if it exists in the MinGW prefix. That is also
# what keeps Windows' own DLLs out: kernel32 and the api-ms-win-* stubs are not
# there, so they are never matched and never copied.
resolve_dll () {
  local name="$1" hit
  [[ -f "$prefix/bin/$name" ]] && { printf '%s\n' "$prefix/bin/$name"; return 0; }
  hit="$(find "$prefix/bin" -maxdepth 1 -iname "$name" -print -quit 2>/dev/null || true)"
  [[ -n "$hit" ]] && { printf '%s\n' "$hit"; return 0; }
  return 1
}

have_in_dist () {
  [[ -n "$(find "$DIST" -maxdepth 1 -iname "$1" -print -quit 2>/dev/null || true)" ]]
}

pass=0
while :; do
  pass=$((pass + 1))
  added=0
  mapfile -t bins < <(find "$DIST" \( -iname '*.dll' -o -iname '*.exe' \) 2>/dev/null)
  for bin in "${bins[@]}"; do
    while IFS= read -r name; do
      [[ -n "$name" ]] || continue
      have_in_dist "$name" && continue
      src="$(resolve_dll "$name" || true)"
      [[ -n "$src" ]] || continue
      cp -n "$src" "$DIST/" 2>/dev/null || true
      added=$((added + 1))
    done < <("$OBJDUMP" -p "$bin" 2>/dev/null | awk '/DLL Name:/ {print $3}')
  done
  echo "    pass $pass: +$added DLL(s), $(find "$DIST" -maxdepth 1 -iname '*.dll' | wc -l) total"
  [[ $added -eq 0 ]] && break
  # The closure is finite and every pass only adds, so this terminates on its
  # own; the cap is here so a surprise cannot spin forever in a build script.
  [[ $pass -ge 16 ]] && { echo "    stopping after $pass passes" >&2; break; }
done

# GTK's runtime data. Missing icons are the usual symptom of skipping this:
# the window comes up with every symbolic icon blank.
mkdir -p "$DIST/share/glib-2.0/schemas" "$DIST/share/icons"
cp -rn "$prefix/share/icons/Adwaita"           "$DIST/share/icons/"        2>/dev/null || true
cp -rn "$prefix/share/icons/hicolor"           "$DIST/share/icons/"        2>/dev/null || true
cp -rn "$prefix/share/glib-2.0/schemas/"*.compiled "$DIST/share/glib-2.0/schemas/" 2>/dev/null || true

echo "==> $DIST ready (installer packaging is not implemented)"

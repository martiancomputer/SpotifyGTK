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
CROSS="${SPOTIFYGTK_CROSS_FILE:-$HERE/cross/mingw-w64-x86_64.ini}"

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
  setup_args=(--cross-file "$CROSS" --native-file "$HERE/cross/linux-build-tools.ini")
  project_root="$(cd "$HERE/../.." && pwd)"
  setup_args+=("-Dc_args=['-D_UCRT','-ffile-prefix-map=$project_root=.','-fmacro-prefix-map=$project_root=.','-fdebug-prefix-map=$project_root=.']")
fi

if [[ -n "${SPOTIFYGTK_EXTRA_LINK_DIR:-}" ]]; then
  setup_args+=("-Dc_link_args=['-mwindows','-L${SPOTIFYGTK_EXTRA_LINK_DIR}']")
fi
setup_args+=(--buildtype=release)

if [[ ! -f "$BUILD/meson-private/coredata.dat" ]]; then
  meson setup "$BUILD" "$SRC" "${setup_args[@]}"
else
  # Reconfiguring keeps an existing build dir usable after option changes.
  meson setup --reconfigure "$BUILD" "$SRC" "${setup_args[@]}"
fi

meson compile -C "$BUILD" spotify-native
echo "==> built: $BUILD/src/spotify-native.exe"

$bundle || exit 0

# ---------------------------------------------------------------------------
# Bundle: a GTK4 app on Windows needs far more than its own executable.
# ---------------------------------------------------------------------------
echo "==> bundling runtime into $DIST"
mkdir -p "$DIST"

# Meson does not export its install prefix to this script. Pick the native
# MSYS2 prefix automatically, while still allowing callers and Linux cross
# builds to override it explicitly.
if [[ -z "${prefix:-}" ]]; then
  if [[ -n "${MINGW_PREFIX:-}" ]]; then
    prefix="$MINGW_PREFIX"
  elif [[ -d /ucrt64 ]]; then
    prefix=/ucrt64
  elif [[ -d /mingw64 ]]; then
    prefix=/mingw64
  else
    prefix=/usr/x86_64-w64-mingw32
  fi
fi
[[ -d "$prefix/bin" ]] || {
  echo "error: MinGW prefix '$prefix' has no bin directory" >&2
  exit 1
}

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
cp -n "$prefix/lib/gio/modules/giomodule.cache" "$DIST/lib/gio/modules/" 2>/dev/null || true
cp -rn "$prefix/lib/gdk-pixbuf-2.0"   "$DIST/lib/"             2>/dev/null || true
if [[ ! -s "$DIST/lib/gio/modules/giomodule.cache" ]]; then
  if [[ -n "${MSYSTEM:-}" ]]; then
    "$prefix/bin/gio-querymodules.exe" "$DIST/lib/gio/modules"
  elif command -v wine >/dev/null 2>&1; then
    WINEPREFIX="${SPOTIFYGTK_WINE_PREFIX:-$HERE/tools-build/wine-prefix}" WINEDEBUG=-all \
      wine "$prefix/bin/gio-querymodules.exe" "$DIST/lib/gio/modules"
  fi
fi

# Generate a relocatable image-loader cache. The query utility emits relative
# module paths on Windows; discard its diagnostic comments, which contain the
# build machine's absolute path. Wine runs only this command-line utility when
# cross-building, not the application or a Windows virtual machine.
pixbuf_cache="$DIST/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
if [[ -n "${MSYSTEM:-}" ]]; then
  "$prefix/bin/gdk-pixbuf-query-loaders.exe" | sed '/^#/d;s/\r$//' > "$pixbuf_cache"
elif command -v wine >/dev/null 2>&1; then
  WINEPREFIX="${SPOTIFYGTK_WINE_PREFIX:-$HERE/tools-build/wine-prefix}" WINEDEBUG=-all \
    wine "$prefix/bin/gdk-pixbuf-query-loaders.exe" | sed '/^#/d;s/\r$//' > "$pixbuf_cache"
elif [[ -s "$pixbuf_cache" ]]; then
  sed '/^#/d;s/\r$//' "$pixbuf_cache" > "$pixbuf_cache.tmp"
  mv "$pixbuf_cache.tmp" "$pixbuf_cache"
else
  echo 'error: a relocatable pixbuf loader cache is required; install Wine or supply one' >&2
  exit 1
fi
[[ -s "$pixbuf_cache" ]] || { echo 'error: image loader cache is empty' >&2; exit 1; }

# A name is ours to ship only if it exists in the MinGW prefix. That is also
# what keeps Windows' own DLLs out: kernel32 and the api-ms-win-* stubs are not
# there, so they are never matched and never copied. Build the case-insensitive
# index once: repeatedly running find for every import of every DLL made later
# fixpoint passes take many minutes on an otherwise fast Windows guest.
declare -A prefix_dlls=()
while IFS= read -r -d '' dll; do
  name="$(basename "$dll")"
  prefix_dlls["${name,,}"]="$dll"
done < <(find "$prefix/bin" -maxdepth 1 -type f -iname '*.dll' -print0)

resolve_dll () {
  local name="$1" hit="${prefix_dlls[${1,,}]:-}"
  [[ -n "$hit" ]] && { printf '%s\n' "$hit"; return 0; }
  return 1
}

have_in_dist () {
  [[ -n "${dist_dlls[${1,,}]:-}" ]]
}

pass=0
while :; do
  pass=$((pass + 1))
  added=0
  declare -A dist_dlls=()
  while IFS= read -r -d '' dll; do
    name="$(basename "$dll")"
    dist_dlls["${name,,}"]="$dll"
  done < <(find "$DIST" -maxdepth 1 -type f -iname '*.dll' -print0)
  mapfile -t bins < <(find "$DIST" \( -iname '*.dll' -o -iname '*.exe' \) 2>/dev/null)
  for bin in "${bins[@]}"; do
    while IFS= read -r name; do
      [[ -n "$name" ]] || continue
      have_in_dist "$name" && continue
      src="$(resolve_dll "$name" || true)"
      [[ -n "$src" ]] || continue
      cp -n "$src" "$DIST/" 2>/dev/null || true
      dist_dlls["${name,,}"]="$DIST/$name"
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
cp -rn "$prefix/share/icons/AdwaitaLegacy"     "$DIST/share/icons/"        2>/dev/null || true
cp -rn "$prefix/share/glib-2.0/schemas/"*.compiled "$DIST/share/glib-2.0/schemas/" 2>/dev/null || true
cp -f "$prefix/share/glib-2.0/schemas/"*.xml "$DIST/share/glib-2.0/schemas/"
glib-compile-schemas "$DIST/share/glib-2.0/schemas"

# Fonts are private application resources, not fonts installed on the user's
# machine. A release can supply a licensed font tree (including its notices)
# instead of relying on the build machine's desktop fonts.
font_source="${SPOTIFYGTK_BUNDLE_FONTS:-$prefix/share/fonts}"
[[ -d "$font_source" ]] || {
  echo "error: no bundled fonts; set SPOTIFYGTK_BUNDLE_FONTS to a licensed font tree" >&2
  exit 1
}
mkdir -p "$DIST/share/fonts" "$DIST/etc/fonts" "$DIST/share/licenses"
cp -R "$font_source/." "$DIST/share/fonts/"
cp "$HERE/fonts.conf" "$DIST/etc/fonts/fonts.conf"
if [[ -d "$prefix/share/licenses" ]]; then
  cp -R "$prefix/share/licenses/." "$DIST/share/licenses/"
fi
cp "$HERE/../../LICENSE" "$DIST/share/licenses/SpotifyGTK-LICENSE"
cp "$HERE/../../THIRD_PARTY_LICENSES" "$DIST/share/licenses/SpotifyGTK-THIRD-PARTY-LICENSES"
if LC_ALL=C grep -aqF 'gtk4-enhanced-frame-policy-version' "$DIST/libgtk-4-1.dll"; then
  gtk_bundle_source="${GTK_ENHANCED_SOURCE:-$HERE/../../dependencies/gtk4-enhanced}"
  mkdir -p "$DIST/share/licenses/GTK4-Enhanced"
  for gtk_notice in COPYING AUTHORS UPSTREAM.md FRAME_POLICY.md; do
    [[ -s "$gtk_bundle_source/$gtk_notice" ]] || {
      echo "error: missing enhanced GTK source notice: $gtk_notice" >&2; exit 1;
    }
    cp "$gtk_bundle_source/$gtk_notice" "$DIST/share/licenses/GTK4-Enhanced/"
  done
  cp "$HERE/ENHANCED-GTK.md" "$DIST/share/licenses/GTK4-Enhanced/WINDOWS.md"
  {
    printf 'Corresponding source: https://github.com/martiancomputer/GTK4-Enhanced\nRevision: '
    git -C "$gtk_bundle_source" rev-parse HEAD
  } > "$DIST/share/licenses/GTK4-Enhanced/SOURCE.txt"
fi
if [[ -f "$prefix/../runtime-packages.json" ]]; then
  cp "$prefix/../runtime-packages.json" "$DIST/share/licenses/runtime-packages.json"
fi

# GnuTLS in the MSYS2 glib-networking module is compiled with the build
# environment's absolute trust-store path.  That path is not present when
# this directory is copied to another Windows machine, so the portable app
# would otherwise reach the OAuth callback and then fail its token exchange
# with "Unacceptable TLS certificate".  The application attaches this file to
# every libsoup session; keep it under dist/etc so it follows the executable.
if [[ -s "$prefix/etc/ssl/certs/ca-bundle.crt" ]]; then
  mkdir -p "$DIST/etc/ssl/certs"
  cp -f "$prefix/etc/ssl/certs/ca-bundle.crt" "$DIST/etc/ssl/certs/ca-bundle.crt"
  echo "==> bundled CA database: etc/ssl/certs/ca-bundle.crt"
elif [[ -z "${MSYSTEM:-}" && -s /etc/ssl/certs/ca-certificates.crt ]]; then
  # Cross-build runners already have the distro's public trust bundle. The
  # MSYS2 archive ships empty placeholders until its Windows post-install hook
  # runs, so use the host's standard public CA set rather than a customised
  # developer trust store or an uninitialized package database.
  mkdir -p "$DIST/etc/ssl/certs"
  cp -f /etc/ssl/certs/ca-certificates.crt "$DIST/etc/ssl/certs/ca-bundle.crt"
  echo "==> bundled runner CA database: etc/ssl/certs/ca-bundle.crt"
elif [[ -z "${MSYSTEM:-}" ]] && command -v wine >/dev/null 2>&1; then
  # Repository archives contain an empty placeholder until the ca-certificates
  # post-install hook runs. Extract the Mozilla anchors using the package's
  # own trust tool, never the build machine's potentially customised CA set.
  mkdir -p "$DIST/etc/ssl/certs"
  WINEPREFIX="${SPOTIFYGTK_WINE_PREFIX:-$HERE/tools-build/wine-prefix}" WINEDEBUG=-all \
    wine "$prefix/bin/trust.exe" extract --format=pem-bundle \
      --filter=ca-anchors --overwrite --purpose server-auth \
      "$DIST/etc/ssl/certs/ca-bundle.crt"
  [[ -s "$DIST/etc/ssl/certs/ca-bundle.crt" ]] || {
    echo 'error: extracting the public CA database failed' >&2; exit 1;
  }
else
  echo 'error: public CA database missing; run update-ca-trust in MSYS2' >&2
  exit 1
fi

echo "==> $DIST ready for package-msix.sh"

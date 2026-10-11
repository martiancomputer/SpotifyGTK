#!/usr/bin/env bash
# Linux-only experiment: never replace the system GTK or downgrade libadwaita.
set -euo pipefail

GTK_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GTK_PROJECT_DIR="$(cd "$GTK_SCRIPT_DIR/.." && pwd)"
GTK_ENHANCED_SOURCE="${GTK_ENHANCED_SOURCE:-$GTK_PROJECT_DIR/dependencies/gtk4-enhanced}"
GTK_ENHANCED_BUILD="${GTK_ENHANCED_BUILD:-$GTK_PROJECT_DIR/_build/gtk4-enhanced}"
GTK_ENHANCED_PREFIX="${GTK_ENHANCED_PREFIX:-$GTK_PROJECT_DIR/_stage/gtk4-enhanced/usr}"
GTK_CONSUMER_BUILD="${GTK_CONSUMER_BUILD:-$GTK_PROJECT_DIR/apps/spotify-native/enhanced-build}"
GTK_BUILD_JOBS="${GTK_BUILD_JOBS:-4}"
GTK_ENHANCED_SOURCE="$(realpath -m "$GTK_ENHANCED_SOURCE")"
GTK_ENHANCED_PREFIX="$(realpath -m "$GTK_ENHANCED_PREFIX")"

if [[ "$(uname -s)" != Linux ]]; then
  echo 'The enhanced runtime experiment is currently Linux-only.' >&2
  exit 1
fi
if [[ ! -f "$GTK_ENHANCED_SOURCE/FRAME_POLICY.md" ]]; then
  echo 'Initialize the pinned submodule: git submodule update --init dependencies/gtk4-enhanced' >&2
  exit 1
fi
if [[ "$GTK_ENHANCED_PREFIX" != "$GTK_PROJECT_DIR/"* && "$GTK_ENHANCED_PREFIX" != "$GTK_ENHANCED_SOURCE/"* ]]; then
  echo 'Use an isolated staging prefix inside the project or fork checkout.' >&2
  exit 1
fi

gtk_native_args=()
if [[ -n "${GTK_ENHANCED_NATIVE_FILE:-}" ]]; then
  gtk_native_args=(--native-file "$GTK_ENHANCED_NATIVE_FILE")
fi
gtk_setup_args=()
[[ ! -f "$GTK_ENHANCED_BUILD/meson-private/coredata.dat" ]] || gtk_setup_args=(--reconfigure)
meson setup "${gtk_setup_args[@]}" "$GTK_ENHANCED_BUILD" "$GTK_ENHANCED_SOURCE" \
  "${gtk_native_args[@]}" --prefix="$GTK_ENHANCED_PREFIX" --libdir=lib \
  --buildtype=debugoptimized --wrap-mode=nofallback \
  -Ddocumentation=false -Dman-pages=false -Dintrospection=disabled \
  -Dbuild-demos=false -Dbuild-examples=false \
  -Dbuild-tests=false -Dbuild-testsuite=false
meson compile -C "$GTK_ENHANCED_BUILD" -j "$GTK_BUILD_JOBS"
meson install -C "$GTK_ENHANCED_BUILD" --no-rebuild

export PKG_CONFIG_PATH="$GTK_ENHANCED_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [[ "$(pkg-config --variable=libdir gtk4)" != "$GTK_ENHANCED_PREFIX/lib" ]]; then
  echo 'GTK dependency resolution escaped the isolated prefix.' >&2
  exit 1
fi
gtk_setup_args=()
[[ ! -f "$GTK_CONSUMER_BUILD/meson-private/coredata.dat" ]] || gtk_setup_args=(--reconfigure)
meson setup "${gtk_setup_args[@]}" "$GTK_CONSUMER_BUILD" "$GTK_PROJECT_DIR/apps/spotify-native" \
  --native-file "$GTK_PROJECT_DIR/apps/spotify-native/build-profiles/stable.ini" \
  "-Dc_link_args=-Wl,-rpath,$GTK_ENHANCED_PREFIX/lib"
meson compile -C "$GTK_CONSUMER_BUILD" -j "$GTK_BUILD_JOBS"

printf 'Built with GTK %s; libadwaita %s (host runtime retained).\n' \
  "$(pkg-config --modversion gtk4)" "$(pkg-config --modversion libadwaita-1)"
echo 'Launch with packaging/run-enhanced-gtk.sh; the stock build is unchanged.'

#!/usr/bin/env bash
# Cross-build the pinned fork without replacing the downloaded stock SDK.
set -euo pipefail

gtk_windows_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
gtk_project_root="$(cd "$gtk_windows_root/../.." && pwd)"
gtk_source="${GTK_ENHANCED_SOURCE:-$gtk_project_root/dependencies/gtk4-enhanced}"
gtk_build="${GTK_ENHANCED_BUILD:-$gtk_windows_root/tools-build/gtk4-enhanced-build}"
gtk_base="${SPOTIFYGTK_MSYS_RUNTIME:-$gtk_windows_root/tools-build/msys-runtime}"
gtk_runtime="${SPOTIFYGTK_ENHANCED_RUNTIME:-$gtk_windows_root/tools-build/gtk4-enhanced-runtime}"
gtk_jobs="${GTK_BUILD_JOBS:-4}"

[[ "$(uname -s)" == Linux ]] || { echo 'This cross-build script requires Linux.' >&2; exit 1; }
[[ -s "$gtk_source/FRAME_POLICY.md" && -d "$gtk_base/ucrt64/lib/pkgconfig" ]] || {
  echo 'Initialize the GTK submodule and fetch the UCRT64 SDK first.' >&2; exit 1;
}
[[ "$(realpath -m "$gtk_base")" != "$(realpath -m "$gtk_runtime")" ]] || {
  echo 'Enhanced runtime staging must not overwrite the stock SDK.' >&2; exit 1;
}

export PKG_CONFIG_LIBDIR="$gtk_base/ucrt64/lib/pkgconfig:$gtk_base/ucrt64/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$gtk_base"
gtk_native_args=(--native-file "$gtk_windows_root/cross/linux-build-tools.ini")
gtk_tool_args=()
if [[ -n "${GTK_ENHANCED_NATIVE_FILE:-}" ]]; then
  gtk_native_args+=(--native-file "$GTK_ENHANCED_NATIVE_FILE")
  # Meson's GNOME module looks up generator overrides in the host machine
  # even for build-machine tools. This optional file must contain only
  # generator binaries/constants, not a compiler or pkg-config override.
  gtk_tool_args+=(--cross-file "$GTK_ENHANCED_NATIVE_FILE")
fi
gtk_setup_args=()
[[ ! -f "$gtk_build/meson-private/coredata.dat" ]] || gtk_setup_args=(--reconfigure --clearcache)
meson setup "${gtk_setup_args[@]}" "$gtk_build" "$gtk_source" \
  --cross-file "$gtk_windows_root/cross/llvm-mingw-ucrt64-linux.ini" \
  "${gtk_tool_args[@]}" \
  "${gtk_native_args[@]}" --prefix=/ucrt64 --libdir=lib \
  --buildtype=release --wrap-mode=nofallback \
  "-Dc_args=['-D_UCRT','-ffile-prefix-map=$gtk_project_root=.','-ffile-prefix-map=$gtk_source=gtk4-enhanced']" \
  "-Dc_link_args=['-mwindows','-L$gtk_base/ucrt64/lib']" \
  -Ddocumentation=false -Dman-pages=false -Dintrospection=disabled \
  -Dmedia-gstreamer=disabled \
  -Dbuild-demos=false -Dbuild-examples=false \
  -Dbuild-tests=false -Dbuild-testsuite=false
meson compile -C "$gtk_build" -j "$gtk_jobs"

# Reflink when supported, otherwise copy. Never hardlink mutable SDK files:
# installing the fork must not change the original stock GTK DLL or headers.
mkdir -p "$gtk_runtime/ucrt64"
if [[ ! -f "$gtk_runtime/runtime-packages.json" ]]; then
  cp -a --reflink=auto "$gtk_base/ucrt64/." "$gtk_runtime/ucrt64/"
  cp "$gtk_base/runtime-packages.json" "$gtk_runtime/runtime-packages.json"
fi
DESTDIR="$gtk_runtime" meson install -C "$gtk_build" --no-rebuild
cmp "$gtk_build/gtk/libgtk-4-1.dll" "$gtk_runtime/ucrt64/bin/libgtk-4-1.dll"
echo 'Enhanced Win32 GTK staged. Use this runtime as the consumer sysroot and bundle prefix.'

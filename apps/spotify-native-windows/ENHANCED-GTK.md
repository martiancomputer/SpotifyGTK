# Enhanced GTK Windows tester package

## Scope and implementation plan

This experiment targets platform support, compatibility and packaging. The
existing Win32 backend constructs GdkFrameClockIdle and supplies DWM timing
feedback. Build the pinned GTK fork's existing, backend-neutral frame ceiling
for Win32; do not introduce a second scheduler, force redraws or bypass DWM.
Enable the shared frame-rate preference and capability-checked dropdown in the
Windows application. Interface scaling and the initial keybind manager are
shared with Linux and appear under Settings → Interface. The Windows package
loads its bundled Adwaita Sans and Noto Sans faces explicitly through Pango.

Cross-build GTK first with the existing UCRT64 SDK. Stage it over a separate
copy of that SDK, configure the normal Windows application build against the
enhanced sysroot, and assemble a fresh bundle so no old stock GTK DLL survives
through no-clobber copying. Preserve the stock SDK and previous packages.

## Build

From this directory, after installing Python 3, `zstd`, libarchive's
`bsdtar`, and GTK's native Linux build tools:

```sh
python3 fetch-msys-runtime.py tools-build/msys-runtime \
  --cached-database --extra-package vulkan-headers
export PATH="$(bash fetch-llvm-mingw.sh):$PATH"
bash build-enhanced-gtk.sh
export prefix="$PWD/tools-build/gtk4-enhanced-runtime/ucrt64"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$PWD/tools-build/gtk4-enhanced-runtime"
export SPOTIFYGTK_CROSS_FILE="$PWD/cross/llvm-mingw-ucrt64-linux.ini"
export SPOTIFYGTK_BUNDLE_FONTS="<licensed-font-tree>"
windows_project_root="$(cd ../.. && pwd)"
meson setup --reconfigure --clearcache build ../spotify-native \
  --cross-file "$SPOTIFYGTK_CROSS_FILE" --native-file cross/linux-build-tools.ini \
  --buildtype=release -Dtests=false -Dverbose_logging=false \
  "-Dc_args=['-D_UCRT','-ffile-prefix-map=$windows_project_root=.']"
bash build.sh --bundle
x86_64-w64-mingw32-strip --strip-unneeded \
  build/dist/spotify-native.exe build/dist/libgtk-4-1.dll
SPOTIFYGTK_REQUIRE_ENHANCED_GTK=1 SPOTIFYGTK_MSIX_VERSION=0.1.0.1 \
  bash package-msix.sh
```

`GTK_ENHANCED_NATIVE_FILE` can select additional local Meson generator tools,
without committing machine paths. That file must contain only generator
binaries/constants, not compiler or pkg-config overrides. Build and stage
overrides are documented in the script. Use a fresh bundle directory and a
new MSIX version/output each time. The enhanced-runtime packaging check rejects
a stock DLL or an application lacking the private frame-policy integration.
Reuse the existing private `.signing/` identity; distribute only its public CER.

## Verification and fallback

Compare the Win32 DLL exports with the same upstream version/backend, verify
the bundled DLL matches the newly built fork, inspect the PE dependency
closure, and verify the signed MSIX and certificate identity. Keep all native
build paths, secrets, logs and private signing files out of the deliverable.
Ship GTK's license notices and corresponding modified source information.

Headless settings tests cover preference persistence and validation. These
checks do not certify physical presentation, Windows installation, scrolling,
VRR, mixed-DPI or multi-monitor behavior. The tester must exercise Default,
30, 60, 120, 144, 240 and 360 with Windows configured to a supported display
rate. Report compositor/monitor limits honestly. The choices are experimental
rendering ceilings, not promises that a display presents that many frames.

Default preserves upstream pacing. A stock runtime keeps the control disabled;
the original SDK and earlier signed package remain available for rollback.

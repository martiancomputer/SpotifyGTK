# `spotify-native-windows`

The Windows target for `spotify-native`. **It contains no source of its own** —
it holds the toolchain files, build script and packaging for building the
existing `../spotify-native` tree for Windows.

That is deliberate. A survey of the tree before this port found exactly **one**
POSIX-only header (`sys/utsname.h`) and **zero** raw syscalls — every socket,
thread, timer and file operation already goes through GLib/GIO, which is
cross-platform. Forking 97 source files to change two of them would mean every
future fix (the engine's thread-affinity bug, the resampler, the EQ) landing
twice and drifting apart. So the platform differences live behind `G_OS_WIN32`
in the shared tree, and this directory is the build target.

Meson refuses `files()` paths that escape the project root, so this is not a
second Meson project pointing at the first — it configures the *same* project
into a build directory here.

## What actually differs on Windows

| Piece | Where | Notes |
|---|---|---|
| Audio output | `../spotify-native/src/audio/output_wasapi.c` | Real WASAPI shared-mode backend. The POSIX backends are not compiled on Windows and vice versa (`src/audio/meson.build`). |
| Client-token platform block | `../spotify-native/src/spotify/clienttoken.c` | `NativeDesktopWindowsData` (oneof field 4, integers) instead of `NativeDesktopLinuxData` (field 5, uname strings). Not cosmetic — the wrong shape returns HTTP 400. |
| Backend probe order | `../spotify-native/src/audio/output.c` | WASAPI is the whole candidate list, not a fallback. |
| Link deps | `../spotify-native/meson.build` | `ole32` for COM; PulseAudio/ALSA/PipeWire are not looked for. |

## Building and running it on Windows

Requires **Windows 10 or 11, 64-bit**, about 3 GB of disk, and a **Spotify
Premium account** — free accounts cannot play through this client and never
will, which is a server-side gate rather than a missing feature. See the
[main README](../../README.md#requirements).

Everything below is done in MSYS2. It is a build environment, not something
that stays running afterwards.

### 1. Install MSYS2

Download and run the installer from <https://www.msys2.org>. Accept the
defaults.

### 2. Open the UCRT64 shell

From the Start menu, open **"MSYS2 UCRT64"**.

This matters more than it looks: the installer creates several shells
(MSYS, MINGW64, UCRT64, CLANG64) and they are *not* interchangeable. Packages
installed in one are invisible to another, and the whole build is written
against UCRT64. If the prompt does not say `UCRT64` in magenta, close it and
open the right one.

### 3. Install the toolchain and libraries

Paste this in one go. It is the same list CI installs on every push, so it is
known to work rather than merely plausible:

```bash
pacman -S --needed git \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-meson \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-glib2 \
  mingw-w64-ucrt-x86_64-gtk4 \
  mingw-w64-ucrt-x86_64-libadwaita \
  mingw-w64-ucrt-x86_64-libsoup3 \
  mingw-w64-ucrt-x86_64-json-glib \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-libogg \
  mingw-w64-ucrt-x86_64-libvorbis
```

That pulls in around 110 packages and takes a few minutes.

### 4. Get the source and build it

```bash
git clone https://github.com/martiancomputer/SpotifyGTK.git
cd SpotifyGTK/apps/spotify-native
meson setup build
meson compile -C build
```

No cross file and no extra options: on Windows the audio backend is chosen
from `host_machine`, so WASAPI is selected for you.

MSYS2 currently ships GTK 4.22.4, which is exactly what the interface is laid
out against, so the version floor is satisfied. If a future MSYS2 update ships
something older, `meson setup` says so and names the flag to override it.

### 5. Run it

```bash
./build/src/spotify-native.exe
```

**Run it from the UCRT64 shell.** Double-clicking the `.exe` in Explorer will
fail to start: it needs the GTK DLLs, and only this shell has them on its PATH.
To get something that runs anywhere, see [Packaging](#packaging) below.

On first run a browser opens for you to sign in to Spotify. The token is stored
in your user profile and reused, so this happens once.

### If something goes wrong

| What you see | What it is |
|---|---|
| `bash: pacman: command not found` | Not an MSYS2 shell. Open "MSYS2 UCRT64" from the Start menu. |
| `Dependency gtk4 found: NO` | Step 3 was run in the wrong shell — most often "MSYS2 MSYS" rather than UCRT64. Packages do not carry across. |
| The `.exe` exits immediately with no message when double-clicked | Expected outside the UCRT64 shell; the GTK DLLs are not on PATH. Run it from the shell, or build a bundle. |
| `TLS support is not available` on every request | A bundle missing `lib/gio/modules/` (glib-networking). It reads like a network fault and is not. |
| Sign-in succeeds but nothing plays | Check the account is Premium. The audio-key exchange is refused for free accounts, for every format. |

A log of each run is written to `spotify-native.log` beside the executable,
with the previous run kept as `spotify-native.prev.log`. That is the thing to
attach to a bug report.

## Cross-compiling from Linux

Needs `mingw-w64-gcc` plus mingw builds of the dependencies (on Arch, the
`mingw-w64-*` AUR packages). This gets you a binary and, more usefully, catches
every compile error without a Windows machine:

```bash
./build.sh                     # configures + compiles using cross/mingw-w64-x86_64.ini
./build.sh --bundle            # also collects the DLL closure into build/dist
```

## Packaging

A GTK4 application on Windows needs more than its own `.exe`: the DLL closure,
the Adwaita icon theme (the UI uses `*-symbolic` icons throughout), gdk-pixbuf
loaders, and compiled GSettings schemas. `./build.sh --bundle` assembles those
into `build/dist/`. Turning that into an installer (NSIS/MSIX) is not done.

The DLL closure is walked to a fixpoint rather than in one pass, because the
modules are loaded with dlopen and nothing links them: a walk starting at the
executable cannot reach `lib/gio/modules` or the gdk-pixbuf loaders, and so
misses their dependencies as well. Those are copied first and everything in
`dist` is then re-walked until a pass adds nothing.

## Status — honest

### Verified, on real Windows

Built under **MSYS2 UCRT64** on a Windows 11 guest and run there. Everything
below was observed, not inferred:

- **The build.** `meson setup build ../spotify-native && meson compile -C build`
  produces a working `spotify-native.exe`. This is also what validated the
  `host_machine.system()` gating in `meson.build` and `src/audio/meson.build` —
  a cross `meson setup` never reaches those branches, so MSYS2 is what exercised
  them.
- **The client-token platform block, against live servers.** `NativeDesktopWindowsData`
  returns HTTP 200 (`clienttoken: sending request (windows build=26200 …)`).
  This was the highest-risk unverifiable piece: a wrong platform message fails
  as an opaque HTTP 400 with an empty body.
- **Sign-in**, through login5 to a usable bearer token.
- **Streaming**, end to end: AP handshake, metadata, CDN resolution, audio key,
  decrypt, decode.
- **WASAPI playback.** Audio actually comes out. The buffer handling, COM
  apartment pairing and drain loop are exercised.
- **Packaging.** `build/dist/` runs as a portable directory on a machine with no
  MSYS2 and no GTK installed.

Concrete catches from the port, each of which broke something real:

- **`AUDCLNT_STREAMFLAGS_AUTOCONVERT_PCM` and `AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`
  are not defined in mingw-w64's `audioclient.h`.** The `#ifndef` fallbacks in
  `output_wasapi.c` are what make it build at all.
- **`g_app_info_launch_default_for_uri()` asserts on Windows** when handed a NULL
  launch context, so OAuth sign-in could not open a browser. `ShellExecuteW` is
  used instead (`native_auth.c`).
- **A GTK bundle needs `lib/gio/modules/`.** Without glib-networking there, every
  HTTPS request fails with "TLS support is not available" — which looks like a
  network problem and is not. Loadable modules also carry their own dependency
  chains, so the DLL closure has to be computed to a fixpoint rather than in one
  pass over the executables: 81 DLLs before, 109 after.

### Not verified / known wrong

- `g_chmod (path, 0600)` on the stored token is a silent no-op on Windows. The
  file sits in the user profile, which is ACL'd, but the "chmod 600" claim in
  `native_auth.c`'s header is not true there. DPAPI (`CryptProtectData`) is the
  proper fix and is not done.
- `cairo_select_font_face ("Sans", …)` in `ui/eq_graph.c` resolves via
  fontconfig on Linux; on Windows Cairo falls back to whatever it picks.
  Rendering that text through Pango instead would be more correct.
- No installer. `build/dist/` is a portable directory; turning it into NSIS or
  MSIX is not done.
- The cross-compile path below is a compile check only — no binary produced that
  way has been run.

### Reproducing the compile check without MSYS2

`meson setup` cannot run, but the two Windows files can still be compiled
directly with only `mingw-w64-gcc` installed, by shimming the handful of GLib
types they use. That is how the results above were produced; it catches header,
macro and API-name errors, which is most of what a port gets wrong first.

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

## Building natively on Windows (recommended)

Under **MSYS2 UCRT64**. MSVC is not supported — GLib/GTK on MSVC is possible but
buys nothing here.

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-{gcc,meson,ninja,pkgconf} \
  mingw-w64-ucrt-x86_64-{glib2,gtk4,libadwaita,libsoup3,json-glib} \
  mingw-w64-ucrt-x86_64-{openssl,libogg,libvorbis}

meson setup build ../spotify-native
meson compile -C build
```

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

## Status — honest

### Verified

Both Windows-only source paths were cross-compiled against the real mingw-w64
Windows headers (`x86_64-w64-mingw32-gcc` 16.1, `-Wall -Wextra`, no warnings):

- `output_wasapi.c` compiles **and links** into a Windows `.exe` with `-lole32`.
  That proves the `COBJMACROS` call forms, the `WAVEFORMATEX`/`eRender`/
  `AUDCLNT_SHAREMODE_SHARED` names, and that `INITGUID` resolves
  `CLSID_MMDeviceEnumerator` / `IID_IAudioClient` / `IID_IAudioRenderClient` /
  `IID_ISimpleAudioVolume` without linking a separate uuid library.
- The Windows arm of the client-token platform block compiles (extracted
  verbatim from `clienttoken.c`): `RtlGetVersion` via `GetProcAddress`,
  `OSVERSIONINFOEXW`, and the machine-constant selection.

One concrete catch from doing this: **`AUDCLNT_STREAMFLAGS_AUTOCONVERT_PCM` and
`AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY` are not defined in mingw-w64's
`audioclient.h`.** The `#ifndef` fallbacks in `output_wasapi.c` are what make it
build at all; without them this would not have compiled.

### Not verified

- **Nothing has run on Windows.** Compiling is not the same as working: the WASAPI buffer
  handling, the COM apartment pairing and the drain loop are all unexercised.
- **The Meson platform gating is unvalidated.** A cross `meson setup` fails at
  the very first dependency (`glib-2.0`) because a cross toolchain alone has no
  mingw pkg-config or mingw GLib, so configuration never reaches the
  `host_machine.system()` branches in `meson.build` / `src/audio/meson.build`.
  Building under MSYS2, where the dependencies exist, is what will exercise them.
- **The client-token block needs live servers.** A wrong platform message fails
  as an opaque `HTTP 400` with an empty body, so if Windows sign-in fails that
  way, suspect this first.
- `g_chmod (path, 0600)` on the stored token is a silent no-op on Windows. The
  file sits in the user profile, which is ACL'd, but the "chmod 600" claim in
  `native_auth.c`'s header is not true there. DPAPI (`CryptProtectData`) is the
  proper fix and is not done.
- `cairo_select_font_face ("Sans", …)` in `ui/eq_graph.c` resolves via
  fontconfig on Linux; on Windows Cairo will fall back to whatever it picks.
  Rendering that text through Pango instead would be more correct.

### Reproducing the compile check without MSYS2

`meson setup` cannot run, but the two Windows files can still be compiled
directly with only `mingw-w64-gcc` installed, by shimming the handful of GLib
types they use. That is how the results above were produced; it catches header,
macro and API-name errors, which is most of what a port gets wrong first.

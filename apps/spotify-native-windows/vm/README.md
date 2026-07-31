# Windows build VM

A QEMU/KVM Windows guest for building and running `spotify-native` on the
platform it targets. Nothing here is required to *build* for Windows — that is
`../build.sh` — this exists to answer the questions a cross-compile cannot:
does it run, does WASAPI make sound, does the client-token block survive a live
server, and does the GTK4 runtime come up with its icons.

## Why not WinApps

WinApps is the nicer front end, but it drives a Windows container via
`docker`/`podman` and neither is installed here, while `qemu-system-x86_64`,
`qemu-img` and OVMF already are. Plain QEMU needs no new packages and no root.
If you install WinApps later, this VM is still the thing underneath it.

## One manual step: the ISO

Microsoft's download pages are JS-gated, so no script can fetch them. Grab a
free 90-day evaluation ISO — no product key, no account — from:

<https://www.microsoft.com/en-us/evalcenter/download-windows-11-enterprise>

## Bring it up

```bash
./run-vm.sh install ~/Downloads/Windows11_Eval.iso   # 20-60 min, unattended
./run-vm.sh run                                      # every boot after that
./run-vm.sh ssh                                      # a shell in the guest
```

The install is driven by `unattend/autounattend.xml`: it partitions the disk,
creates a `builder` account, and — the part that matters — installs and starts
**OpenSSH Server**. RDP is forwarded too (`localhost:3389`) but it is only good
for *looking* at the UI; you cannot read a compiler's stderr out of a picture
of a desktop, which is why sshd is what makes this VM useful.

Ports (guest is NAT'd, reachable only from this host):

| Host | Guest | For |
|---|---|---|
| 2222 | 22 | SSH — build output, logs, everything |
| 3389 | 3389 | RDP — screenshots, clicking the UI |

Knobs: `RAM_MB=8192 CPUS=6 DISK_SIZE=80G ./run-vm.sh install ...`

## Provision the toolchain

Once `./run-vm.sh ssh true` succeeds, install MSYS2 and the dependency stack.
From the host:

```bash
./run-vm.sh ssh 'powershell -c "winget install -e --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements"'

./run-vm.sh ssh 'C:\msys64\usr\bin\bash -lc "pacman -Syu --noconfirm"'
./run-vm.sh ssh 'C:\msys64\usr\bin\bash -lc "pacman -S --needed --noconfirm \
  mingw-w64-ucrt-x86_64-{gcc,meson,ninja,pkgconf} \
  mingw-w64-ucrt-x86_64-{glib2,gtk4,libadwaita,libsoup3,json-glib} \
  mingw-w64-ucrt-x86_64-{openssl,libogg,libvorbis} git"'
```

## Build

Copy the tree in over SSH (the guest has no access to the host filesystem —
NAT only, no shared folder is configured):

```bash
cd ../../..                     # repo root
tar czf - apps/spotify-native apps/spotify-native-windows \
  | apps/spotify-native-windows/vm/run-vm.sh ssh \
      'C:\msys64\usr\bin\bash -lc "mkdir -p /c/src && tar xzf - -C /c/src"'
```

Then, in the guest's **UCRT64** environment:

```bash
./run-vm.sh ssh 'C:\msys64\ucrt64.exe -c "cd /c/src/apps/spotify-native-windows && \
  meson setup build ../spotify-native && meson compile -C build"'
```

This is the first thing that exercises the `host_machine.system() == '\''windows'\''`
branches in `meson.build` and `src/audio/meson.build`, which the cross-compile
never reached — it died at the first dependency, since a cross toolchain alone
has no mingw GLib.

## Expect this to break

Nothing in the Windows path has ever run. The likely first failures, roughly in
order:

1. **Meson platform branches** — never executed by anything.
2. **WASAPI at runtime** — it compiles and links, but the buffer handling, the
   COM apartment pairing and the drain loop are all unexercised.
3. **Sign-in returning HTTP 400 with an empty body** — that is the signature of
   a wrong client-token platform message; suspect `clienttoken.c`'s Windows arm
   first (see `../README.md`).
4. **Blank symbolic icons** — the Adwaita icon theme is not part of the build;
   `../build.sh --bundle` is what collects it.

## Notes

- No emulated TPM (`swtpm` is not installed), so the autounattend sets
  Microsoft's documented `LabConfig` keys to skip Windows 11's TPM/Secure Boot
  checks. That is a setup gate, not a licensing one — the install is an
  ordinary evaluation install and still needs a legitimate ISO.
- `win.qcow2`, `OVMF_VARS.fd` and `share/` are build artefacts; keep them out
  of git (a 64 GB disk image in history would be considerably worse than the
  63 MB `perf.data` already purged from this repo once).
- The `builder` account password is in `autounattend.xml` in plain text. It is
  a local NAT'd VM reachable only from this host; if that ever stops being
  true, change it there before first boot.

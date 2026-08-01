#!/usr/bin/env bash
#
# run-vm.sh — bring up the Windows build VM under QEMU/KVM.
#
#   ./run-vm.sh install /path/to/windows.iso   first boot: unattended install
#   ./run-vm.sh run                            subsequent boots
#   ./run-vm.sh ssh [command...]               shell (or one command) in the guest
#
# Why plain QEMU rather than libvirt or the WinApps docker image: virt-install
# and docker/podman are not installed on this host, whereas qemu-system-x86_64,
# qemu-img and OVMF are. This needs nothing further.
#
# The guest is NAT'd, reachable only from this host on forwarded ports:
#   2222 -> 22    (SSH: the useful one -- build output is text)
#   3389 -> 3389  (RDP: only for looking at the UI)
#
# See unattend/autounattend.xml for what the install configures, and README.md
# for what to do once it is up.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Scripts and autounattend.xml are source and live in the repo. The VM's
# runtime state -- a 64 GB disk image, UEFI variables, the guest share -- is
# host infrastructure and deliberately does NOT: a disk image inside a working
# tree is one careless `git add -f` from being unrecoverable, and it drags the
# repo into any backup or sync of the project directory.
VM_DIR="${SPOTIFYGTK_VM_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/spotifygtk-vm}"
mkdir -p "$VM_DIR"

DISK="$VM_DIR/win.qcow2"
NVRAM="$VM_DIR/OVMF_VARS.fd"
SHARE="$VM_DIR/share"
MONITOR="$VM_DIR/monitor.sock"
UNATTEND_DIR="$HERE/unattend"      # source: stays with the repo

DISK_SIZE="${DISK_SIZE:-64G}"
RAM_MB="${RAM_MB:-6144}"
CPUS="${CPUS:-4}"
SSH_PORT=2222
RDP_PORT=3389

# Firmware: Windows 11 requires UEFI, so this is not optional.
find_ovmf () {
  for c in /usr/share/edk2/x64/OVMF_CODE.4m.fd \
           /usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
           /usr/share/OVMF/OVMF_CODE.fd; do
    [[ -f "$c" ]] && { echo "$c"; return; }
  done
  echo "error: no OVMF firmware found (install edk2-ovmf)" >&2; exit 1
}
find_ovmf_vars () {
  for c in /usr/share/edk2/x64/OVMF_VARS.4m.fd \
           /usr/share/edk2-ovmf/x64/OVMF_VARS.fd \
           /usr/share/OVMF/OVMF_VARS.fd; do
    [[ -f "$c" ]] && { echo "$c"; return; }
  done
  echo "error: no OVMF_VARS template found" >&2; exit 1
}

launch () {                       # launch <extra qemu args...>
  [[ -f "$NVRAM" ]] || cp "$(find_ovmf_vars)" "$NVRAM"
  mkdir -p "$SHARE"

  # No swtpm on this host, so no -tpmdev: the autounattend LabConfig keys are
  # what get Windows 11 past its TPM/Secure Boot checks instead.
  exec qemu-system-x86_64 \
    -name spotifygtk-build \
    -machine q35,accel=kvm \
    -cpu host \
    -smp "$CPUS" \
    -m "$RAM_MB" \
    -drive if=pflash,format=raw,readonly=on,file="$(find_ovmf)" \
    -drive if=pflash,format=raw,file="$NVRAM" \
    -drive file="$DISK",if=virtio,format=qcow2,cache=writeback \
    -netdev "user,id=n0,hostfwd=tcp::${SSH_PORT}-:22,hostfwd=tcp::${RDP_PORT}-:3389" \
    -device virtio-net-pci,netdev=n0 \
    -device virtio-balloon \
    -usb -device usb-tablet \
    -vga std \
    -monitor "unix:$MONITOR,server,nowait" \
    "$@"
}

case "${1:-}" in
  install)
    ISO="${2:-}"
    [[ -f "$ISO" ]] || {
      cat >&2 <<EOF
usage: $0 install /path/to/windows.iso

Get an ISO from Microsoft's Evaluation Center (free, 90-day, no key needed):
  https://www.microsoft.com/en-us/evalcenter/download-windows-11-enterprise
The page is JS-gated, so it cannot be fetched from a script.
EOF
      exit 1
    }
    echo "==> VM state in $VM_DIR (override with SPOTIFYGTK_VM_DIR)"
    [[ -f "$DISK" ]] || qemu-img create -f qcow2 "$DISK" "$DISK_SIZE"

    echo "==> installing Windows unattended; this takes 20-60 minutes."
    echo "    watch progress in the QEMU window, or poll:  $0 ssh true"
    # VVFAT serves unattend/ to the guest as a removable drive, which is where
    # Windows Setup looks for autounattend.xml -- equivalent to embedding it in
    # the ISO, without needing xorriso/genisoimage to rebuild one.
    #
    # Read-only on purpose: QEMU's read-write VVFAT is unreliable, and Setup
    # only ever reads this file.
    # Attach the CD explicitly to an AHCI controller. Letting QEMU auto-attach
    # on q35 put it on the implicit SATA bus, where OVMF timed out reading it
    # ("failed to start Boot0002 ... Sata(0,0,0xFFFF,0x0) : Time out") and then
    # fell through the whole boot order to PXE, which looked like a netboot
    # attempt but was just the last entry in the list.
    # Nothing is watching the console to answer "Press any key to boot from
    # CD or DVD", which times out into "No bootable option". Tap it remotely.
    rm -f "$MONITOR"
    "$HERE/press-boot-key.sh" "$MONITOR" &

    launch \
      -boot order=d \
      -device ahci,id=ahci \
      -drive file="$ISO",if=none,id=cd0,media=cdrom,readonly=on \
      -device ide-cd,bus=ahci.0,drive=cd0,bootindex=0 \
      -drive file="fat:$UNATTEND_DIR",format=raw,if=none,id=unattend,readonly=on \
      -device usb-storage,drive=unattend
    ;;

  run)
    [[ -f "$DISK" ]] || { echo "error: no disk at $DISK -- run '$0 install <iso>' first" >&2; exit 1; }
    launch -boot order=c
    ;;

  ssh)
    shift || true
    # StrictHostKeyChecking off: the guest regenerates host keys on reinstall
    # and it is a local NAT'd VM, so pinning them only creates friction.
    exec ssh -p "$SSH_PORT" \
      -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -o LogLevel=ERROR \
      builder@127.0.0.1 "$@"
    ;;

  *)
    sed -n '3,20p' "$0" | sed 's/^# \?//'
    exit 1
    ;;
esac

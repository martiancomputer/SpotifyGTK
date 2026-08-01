#!/usr/bin/env bash
#
# press-boot-key.sh — answer "Press any key to boot from CD or DVD".
#
# Windows install media waits about five seconds for a keypress and then falls
# through to "BdsDxe: No bootable option or device was found". An unattended
# install has nobody watching the console, so this taps Return through the QEMU
# monitor socket for the first ~20 seconds of boot.
#
# Harmless afterwards: the prompt only exists in the ISO's El Torito
# bootloader, and once Setup is running the keys go nowhere.
#
# Usage: press-boot-key.sh /path/to/monitor.sock

set -u
MONITOR="${1:?usage: press-boot-key.sh <monitor.sock>}"

sleep 3
for _ in $(seq 1 10); do
  python3 - "$MONITOR" <<'PY' 2>/dev/null || true
import socket, sys, time
try:
    s = socket.socket(socket.AF_UNIX)
    s.connect(sys.argv[1])
    time.sleep(0.2)
    s.sendall(b"sendkey ret\n")
    time.sleep(0.2)
    s.close()
except OSError:
    pass          # monitor not up yet, or already gone; both fine
PY
  sleep 2
done

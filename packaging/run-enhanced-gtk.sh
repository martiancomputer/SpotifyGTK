#!/usr/bin/env bash
set -euo pipefail

GTK_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GTK_PROJECT_DIR="$(cd "$GTK_SCRIPT_DIR/.." && pwd)"
GTK_ENHANCED_PREFIX="${GTK_ENHANCED_PREFIX:-$GTK_PROJECT_DIR/_stage/gtk4-enhanced/usr}"
GTK_CONSUMER_BUILD="${GTK_CONSUMER_BUILD:-$GTK_PROJECT_DIR/apps/spotify-native/build}"
if [[ "$(uname -s)" != Linux ]]; then
  echo 'The enhanced runtime experiment is currently Linux-only.' >&2
  exit 1
fi
if [[ ! -f "$GTK_ENHANCED_PREFIX/lib/libgtk-4.so.1" || ! -x "$GTK_CONSUMER_BUILD/src/spotify-native" ]]; then
  echo 'Build the isolated runtime first with packaging/build-enhanced-gtk.sh.' >&2
  exit 1
fi
export LD_LIBRARY_PATH="$GTK_ENHANCED_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="$GTK_ENHANCED_PREFIX/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$GTK_CONSUMER_BUILD/src/spotify-native" "$@"

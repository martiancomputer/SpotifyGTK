#!/usr/bin/env bash
# Install the pinned LLVM-MinGW UCRT cross toolchain used by Linux CI.
set -euo pipefail

tool_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools_root="$tool_dir/tools-build"
tool_version=20261006
tool_archive="llvm-mingw-${tool_version}-ucrt-ubuntu-22.04-x86_64.tar.xz"
tool_sha256=5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4
tool_root="$tools_root/llvm-mingw-$tool_version"
tool_compiler="$tool_root/bin/x86_64-w64-mingw32-clang"

mkdir -p "$tools_root"
if [[ ! -x "$tool_compiler" ]]; then
  tool_path="$tools_root/$tool_archive"
  curl -fsSL \
    "https://github.com/mstorsjo/llvm-mingw/releases/download/$tool_version/$tool_archive" \
    -o "$tool_path"
  printf '%s  %s\n' "$tool_sha256" "$tool_path" | sha256sum --check --status || {
    echo 'LLVM-MinGW archive checksum mismatch.' >&2
    exit 1
  }
  mkdir -p "$tool_root"
  tar -xJf "$tool_path" --strip-components=1 -C "$tool_root"
fi

for tool in clang clang++ gcc g++ ar strip windres; do
  [[ -x "$tool_root/bin/x86_64-w64-mingw32-$tool" ]] || {
    echo "LLVM-MinGW is missing its x86_64 compiler tool: $tool" >&2
    exit 1
  }
done
printf 'Using LLVM-MinGW %s (UCRT)\n' "$tool_version" >&2
printf '%s\n' "$tool_root/bin"

#!/usr/bin/env bash
# Build Microsoft's native Linux MSIX packer, pinned to a reviewed revision.
set -euo pipefail
tool_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tools-build"
tool_source="$tool_root/msix-sdk"
tool_build="$tool_root/msix-sdk-build"
tool_revision=25a65f5c1690930813bcc10cdf1d59fa865f2bb1
mkdir -p "$tool_root"
if [[ ! -d "$tool_source/.git" ]]; then
  GIT_LFS_SKIP_SMUDGE=1 git clone --no-checkout \
    https://github.com/microsoft/msix-packaging.git "$tool_source"
fi
git -C "$tool_source" checkout --detach "$tool_revision"
# Current ICU headers require C++17 for their optional C++ wrappers. MSIX
# only needs ICU's C API; disable those wrappers and use Xerces' iconv backend.
cmake -S "$tool_source" -B "$tool_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_CXX_FLAGS=-DU_SHOW_CPLUSPLUS_API=0 \
  -DLINUX=ON -DMSIX_PACK=ON -DUSE_VALIDATION_PARSER=ON \
  -DMSIX_TESTS=OFF -DMSIX_SAMPLES=OFF \
  -Dtranscoder=iconv -Dmessage-loader=inmemory
cmake --build "$tool_build" --target makemsix -j "${SPOTIFYGTK_BUILD_JOBS:-3}"
echo "MSIX packer: $tool_build/bin/makemsix"

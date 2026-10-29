#!/usr/bin/env bash
# Linux-native MSIX packaging and self-signing. No VM or Windows SDK required.
set -euo pipefail
umask 077
package_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
package_version="${SPOTIFYGTK_MSIX_VERSION:-1.1.7.0}"
package_bundle="${SPOTIFYGTK_BUNDLE_DIR:-$package_root/build/dist}"
package_output="${SPOTIFYGTK_MSIX_OUTPUT:-$package_root/out}"
package_signing="${SPOTIFYGTK_SIGNING_DIR:-$package_root/.signing}"
package_tool="${MAKEMSIX:-$package_root/tools-build/msix-sdk-build/bin/makemsix}"
[[ "$package_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo 'MSIX version must have four numeric components.' >&2; exit 1;
}
package_release_version="${package_version%.*}"
package_unsigned="$package_output/.spotifygtk-${package_version}-unsigned.msix"
package_signed="$package_output/spotifygtk-${package_release_version}-release-windows-x86-64.msix"
package_certificate="$package_output/spotifygtk-${package_release_version}-release-windows-x86-64.cer"
if [[ -e "$package_unsigned" || -e "$package_signed" ]]; then
  echo 'Output already exists; increase SPOTIFYGTK_MSIX_VERSION or use a fresh SPOTIFYGTK_MSIX_OUTPUT directory.' >&2
  exit 1
fi
for tool in openssl osslsigncode python3; do command -v "$tool" >/dev/null; done
[[ -x "$package_tool" ]] || { echo 'Set MAKEMSIX to the Linux MSIX SDK tool.' >&2; exit 1; }
for required in spotify-native.exe etc/ssl/certs/ca-bundle.crt \
                share/glib-2.0/schemas/gschemas.compiled etc/fonts/fonts.conf \
                lib/gdk-pixbuf-2.0/2.10.0/loaders.cache; do
  [[ -s "$package_bundle/$required" ]] || { echo "Missing runtime resource: $required" >&2; exit 1; }
done
[[ -d "$package_bundle/share/icons/Adwaita" && -d "$package_bundle/share/fonts/Adwaita" ]] || {
  echo 'Adwaita icons and fonts must be bundled.' >&2; exit 1;
}
for font in Adwaita/AdwaitaSans-Regular.ttf Adwaita/AdwaitaSans-Italic.ttf \
            noto/NotoSans-Regular.ttf noto/NotoSans-Bold.ttf \
            noto/NotoSans-Italic.ttf noto/NotoSans-BoldItalic.ttf; do
  [[ -s "$package_bundle/share/fonts/$font" ]] || {
    echo "Missing required Linux-matching interface font: $font" >&2; exit 1;
  }
done
if [[ "${SPOTIFYGTK_REQUIRE_ENHANCED_GTK:-0}" == 1 ]]; then
  for policy_binary in libgtk-4-1.dll spotify-native.exe; do
    for policy_key in gtk4-enhanced-frame-policy-version gtk4-enhanced-max-fps-v1; do
      LC_ALL=C grep -aqF "$policy_key" "$package_bundle/$policy_binary" || {
        echo "Missing enhanced GTK frame-policy integration: $policy_binary" >&2; exit 1;
      }
    done
  done
fi
if find "$package_bundle" -type f \( -iname '*.log' -o -iname '*.pfx' -o \
  -iname '*.key' -o -iname '*.pem' -o -iname '*.env' -o -iname '*credentials*' \) -print -quit | read -r _; then
  echo 'Private/debug files found in the runtime bundle; refusing to package.' >&2
  exit 1
fi
# Catch development-path fallbacks and debug strings before a release leaves
# the build machine, including UTF-16 strings in Windows executables.
package_workspace="$(cd "$package_root/../.." && pwd)"
python3 - "$package_bundle" "$package_workspace" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
workspace = sys.argv[2]
patterns = [path.encode(encoding)
            for path in (workspace, workspace.replace('/', '\\'))
            for encoding in ('utf-8', 'utf-16-le')]
overlap = max(map(len, patterns))
for path in root.rglob('*'):
    if not path.is_file():
        continue
    with path.open('rb') as stream:
        tail = b''
        while block := stream.read(1024 * 1024):
            data = tail + block
            if any(pattern in data for pattern in patterns):
                raise SystemExit(f'Private build path found in {path.relative_to(root)}')
            tail = data[-overlap:]
PY
mkdir -p "$package_output" "$package_signing"
chmod 700 "$package_signing"
package_stage="$(mktemp -d "$package_output/stage.XXXXXXXX")"
cp -R "$package_bundle/." "$package_stage/"
mkdir -p "$package_stage/Assets"
# MSIX manifests require logo PNGs. Generate transparent placeholders until
# the finalized visual identity is ready; do not bundle the temporary icon.
python3 - "$package_stage/Assets" <<'PY'
from pathlib import Path
import struct
import sys
import zlib

assets = Path(sys.argv[1])
def chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
for name, size in [('StoreLogo.png', 50), ('Square44x44Logo.png', 44),
                   ('Square150x150Logo.png', 150), ('Square44x44Logo.scale-200.png', 88),
                   ('Square150x150Logo.scale-200.png', 300)]:
    width = height = size
    # RGBA scanlines, fully transparent; no rasterized branding is included.
    scanline = b'\x00' + bytes(width * 4)
    raw = scanline * height
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    (assets / name).write_bytes(png)
PY
sed "s/@VERSION@/$package_version/g" "$package_root/AppxManifest.xml.in" > "$package_stage/AppxManifest.xml"

# Generate once and reuse for upgrades. Only the public certificate is copied
# to the distributable output. The private key never enters the package.
package_key="$package_signing/private-key.pem"
package_cert="$package_signing/certificate.pem"
if [[ ! -e "$package_key" && ! -e "$package_cert" ]]; then
  openssl req -x509 -newkey rsa:3072 -nodes -sha256 -days 1095 \
    -subj '/CN=SpotifyGTK' -keyout "$package_key" -out "$package_cert" \
    -addext 'basicConstraints=critical,CA:FALSE' \
    -addext 'keyUsage=critical,digitalSignature' \
    -addext 'extendedKeyUsage=codeSigning'
fi
[[ -s "$package_key" && -s "$package_cert" ]] || { echo 'Incomplete signing identity.' >&2; exit 1; }
openssl x509 -in "$package_cert" -checkend 86400 -noout
openssl x509 -in "$package_cert" -outform DER -out "$package_certificate"
"$package_tool" pack -d "$package_stage" -p "$package_unsigned"
osslsigncode sign -certs "$package_cert" -key "$package_key" -h sha256 \
  -n SpotifyGTK -in "$package_unsigned" -out "$package_signed"
osslsigncode verify -CAfile "$package_cert" -in "$package_signed"
cp "$package_root/INSTALL-MSIX.md" "$package_output/INSTALL-MSIX.md"
(
  cd "$package_output"
  sha256sum "$(basename "$package_signed")" "$(basename "$package_certificate")" > SHA256SUMS.txt
)
rm -f "$package_unsigned"
chmod 644 "$package_signed" "$package_certificate" \
  "$package_output/INSTALL-MSIX.md" "$package_output/SHA256SUMS.txt"
echo "Signed MSIX and public certificate: $package_output"
echo 'Keep .signing private. Distribute only the MSIX, CER, instructions and checksums.'

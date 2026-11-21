#!/usr/bin/env bash
# Regenerate TinyDVD's icons from the two source images in data/icons/source:
#   tinydvd.png        1024x1024, the hand holding a disc
#   tinydvd-small.png  256x256, just the disc, for sizes up to 64 px, where the
#                      hand would be an unreadable smudge
# It writes data/icons/png/tinydvd-<N>.png (Linux's hicolor icon theme) and
# data/icons/tinydvd.icns (TinyDVD.app on macOS). Run it on a Mac after
# changing a source image and commit the results: installing uses the
# committed files, so nothing extra is needed to install.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ICONS="${ROOT_DIR}/data/icons"
BIG="${ICONS}/source/tinydvd.png"
SMALL="${ICONS}/source/tinydvd-small.png"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

command -v sips >/dev/null && command -v iconutil >/dev/null || { echo "Run this on macOS (needs sips and iconutil)" >&2; exit 1; }

# Resize the right source to size x size
render() {
    local size="$1" out="$2" src="$BIG"
    if [[ "$size" -le 64 ]]; then src="$SMALL"; fi
    sips -s format png -z "$size" "$size" "$src" --out "$out" >/dev/null
}

mkdir -p "${ICONS}/png"
rm -f "${ICONS}"/png/tinydvd-*.png
for size in 16 24 32 48 64 128 256 512; do
    render "$size" "${ICONS}/png/tinydvd-${size}.png"
done

ICONSET="${WORK}/tinydvd.iconset"
mkdir "$ICONSET"
for size in 16 32 128 256 512; do
    render "$size" "${ICONSET}/icon_${size}x${size}.png"
    render "$((size * 2))" "${ICONSET}/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$ICONSET" -o "${ICONS}/tinydvd.icns"
echo "Wrote ${ICONS}/png and ${ICONS}/tinydvd.icns"

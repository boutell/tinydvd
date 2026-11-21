#!/usr/bin/env bash
# Regenerate data/icons/tinydvd.icns (TinyDVD.app's icon on macOS) from
# data/icons/tinydvd.svg. Run it after changing the SVG, and commit the
# result: installing TinyDVD uses the committed .icns, so only this script
# needs rsvg-convert (brew install librsvg).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SVG="${ROOT_DIR}/data/icons/tinydvd.svg"
OUT="${ROOT_DIR}/data/icons/tinydvd.icns"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

command -v rsvg-convert >/dev/null || { echo "Needs rsvg-convert: brew install librsvg" >&2; exit 1; }

# Render every size straight from the SVG, so small sizes stay sharp
ICONSET="${WORK}/tinydvd.iconset"
mkdir "$ICONSET"
for size in 16 32 128 256 512; do
    double=$((size * 2))
    rsvg-convert -w "$size" -h "$size" "$SVG" -o "${ICONSET}/icon_${size}x${size}.png"
    rsvg-convert -w "$double" -h "$double" "$SVG" -o "${ICONSET}/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$ICONSET" -o "$OUT"
echo "Wrote ${OUT}"

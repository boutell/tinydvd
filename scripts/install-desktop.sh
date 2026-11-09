#!/usr/bin/env bash
# install-desktop.sh: Install tinydvd, icons, MIME types, and desktop entry on GNOME/Freedesktop Linux.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

USER_MODE=0
PREFIX=""
UNINSTALL=0

print_usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --user            Install for the current user only (~/.local) without requiring root/sudo
  --prefix <dir>    Install to custom prefix (default: /usr/local for system, ~/.local for --user)
  --uninstall       Remove tinydvd desktop entries, icons, mime types, and binaries
  -h, --help        Show this help message

Examples:
  sudo ./scripts/install-desktop.sh          # System-wide installation (/usr/local)
  ./scripts/install-desktop.sh --user        # Single-user installation (~/.local)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --user)
            USER_MODE=1
            shift
            ;;
        --prefix)
            PREFIX="$2"
            shift 2
            ;;
        --uninstall)
            UNINSTALL=1
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

# Determine prefix
if [[ -z "$PREFIX" ]]; then
    if [[ "$USER_MODE" -eq 1 ]]; then
        PREFIX="${HOME}/.local"
    elif [[ "$(id -u)" -eq 0 ]]; then
        PREFIX="/usr/local"
    else
        echo "Note: Running as non-root user. Defaulting to user installation (~/.local)."
        echo "To install system-wide, run with: sudo $0"
        echo ""
        USER_MODE=1
        PREFIX="${HOME}/.local"
    fi
fi

BIN_DIR="${PREFIX}/bin"
APP_DIR="${PREFIX}/share/applications"
MIME_DIR="${PREFIX}/share/mime/packages"
ICON_APPS_DIR="${PREFIX}/share/icons/hicolor/scalable/apps"
ICON_MIME_DIR="${PREFIX}/share/icons/hicolor/scalable/mimetypes"

if [[ "$UNINSTALL" -eq 1 ]]; then
    echo "=== Uninstalling tinydvd from ${PREFIX} ==="
    rm -f "${BIN_DIR}/tinydvd" "${BIN_DIR}/tinydvd-player" "${BIN_DIR}/convert-to-tinydvd"
    rm -f "${APP_DIR}/tinydvd.desktop"
    rm -f "${MIME_DIR}/tinydvd.xml"
    rm -f "${ICON_APPS_DIR}/tinydvd.svg"
    rm -f "${ICON_MIME_DIR}/video-x-tinydvd.svg"

    if command -v update-mime-database >/dev/null 2>&1; then
        update-mime-database "${PREFIX}/share/mime" || true
    fi
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "${APP_DIR}" || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -q -t "${PREFIX}/share/icons/hicolor" || true
    fi
    echo "tinydvd uninstalled successfully."
    exit 0
fi

echo "=== Installing tinydvd GNOME / Desktop Integration ==="
echo "Target prefix: ${PREFIX}"

# 1. Ensure binary exists or build it
TINYDVD_BIN=""
if [[ -f "${ROOT_DIR}/build/tinydvd" ]]; then
    TINYDVD_BIN="${ROOT_DIR}/build/tinydvd"
elif [[ -f "${ROOT_DIR}/tinydvd" ]]; then
    TINYDVD_BIN="${ROOT_DIR}/tinydvd"
else
    echo "Building tinydvd..."
    cmake -B "${ROOT_DIR}/build" -S "${ROOT_DIR}"
    cmake --build "${ROOT_DIR}/build"
    TINYDVD_BIN="${ROOT_DIR}/build/tinydvd"
fi

if [[ ! -f "$TINYDVD_BIN" ]]; then
    echo "Error: Failed to find or compile tinydvd binary."
    exit 1
fi

# 2. Create destination directories
mkdir -p "${BIN_DIR}" "${APP_DIR}" "${MIME_DIR}" "${ICON_APPS_DIR}" "${ICON_MIME_DIR}"

# 3. Install binaries
echo "Installing binaries to ${BIN_DIR}..."
install -m 755 "${TINYDVD_BIN}" "${BIN_DIR}/tinydvd"
ln -sf tinydvd "${BIN_DIR}/tinydvd-player"

if [[ -f "${ROOT_DIR}/scripts/convert-to-tinydvd.py" ]]; then
    install -m 755 "${ROOT_DIR}/scripts/convert-to-tinydvd.py" "${BIN_DIR}/convert-to-tinydvd"
fi

# 4. Install Desktop entry and MIME package
echo "Installing desktop entry to ${APP_DIR}..."
install -m 644 "${ROOT_DIR}/data/tinydvd.desktop" "${APP_DIR}/tinydvd.desktop"

echo "Installing MIME package to ${MIME_DIR}..."
install -m 644 "${ROOT_DIR}/data/tinydvd.xml" "${MIME_DIR}/tinydvd.xml"

# 5. Install Icons
echo "Installing icons to ${PREFIX}/share/icons/hicolor..."
install -m 644 "${ROOT_DIR}/data/icons/tinydvd.svg" "${ICON_APPS_DIR}/tinydvd.svg"
install -m 644 "${ROOT_DIR}/data/icons/tinydvd.svg" "${ICON_MIME_DIR}/video-x-tinydvd.svg"

# 6. Update system caches
if command -v update-mime-database >/dev/null 2>&1; then
    echo "Updating MIME database..."
    update-mime-database "${PREFIX}/share/mime" || true
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    echo "Updating desktop database..."
    update-desktop-database "${APP_DIR}" || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    echo "Updating icon cache..."
    gtk-update-icon-cache -f -q -t "${PREFIX}/share/icons/hicolor" || true
fi

# 7. Set default handler for .tdvd MIME types
if command -v xdg-mime >/dev/null 2>&1; then
    echo "Registering tinydvd as default player for .tdvd / .tinydvd..."
    xdg-mime default tinydvd.desktop video/x-tinydvd || true
    xdg-mime default tinydvd.desktop application/x-tdvd || true
    xdg-mime default tinydvd.desktop application/x-tinydvd || true
fi

echo ""
echo "Installation complete!"
echo "Files ending in .tdvd and .tinydvd are now mapped to tinydvd in GNOME / Nautilus."
echo "You can now double-click any .tdvd movie in GNOME Files to launch tinydvd."
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
THUMB_DIR="${PREFIX}/share/thumbnailers"
ICON_APPS_DIR="${PREFIX}/share/icons/hicolor/scalable/apps"
ICON_MIME_DIR="${PREFIX}/share/icons/hicolor/scalable/mimetypes"

if [[ "$UNINSTALL" -eq 1 ]]; then
    echo "=== Uninstalling tinydvd from ${PREFIX} ==="
    rm -f "${BIN_DIR}/tinydvd" "${BIN_DIR}/tinydvd-player" "${BIN_DIR}/convert-to-tinydvd" "${BIN_DIR}/tinydvd-thumbnailer" "${BIN_DIR}/tinydvd-set-thumbnail" "${BIN_DIR}/tinydvd-add-thumbnail"
    rm -f "${APP_DIR}/tinydvd.desktop"
    rm -f "${MIME_DIR}/tinydvd.xml"
    rm -f "${THUMB_DIR}/tinydvd.thumbnailer"
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
mkdir -p "${BIN_DIR}" "${APP_DIR}" "${MIME_DIR}" "${THUMB_DIR}" "${ICON_APPS_DIR}" "${ICON_MIME_DIR}"

# 3. Install binaries
echo "Installing binaries to ${BIN_DIR}..."
install -m 755 "${TINYDVD_BIN}" "${BIN_DIR}/tinydvd"
ln -sf tinydvd "${BIN_DIR}/tinydvd-player"

if [[ -f "${ROOT_DIR}/scripts/convert-to-tinydvd.py" ]]; then
    install -m 755 "${ROOT_DIR}/scripts/convert-to-tinydvd.py" "${BIN_DIR}/convert-to-tinydvd"
fi

if [[ -f "${ROOT_DIR}/scripts/tinydvd-thumbnailer.py" ]]; then
    install -m 755 "${ROOT_DIR}/scripts/tinydvd-thumbnailer.py" "${BIN_DIR}/tinydvd-thumbnailer"
fi

if [[ -f "${ROOT_DIR}/scripts/tinydvd-set-thumbnail.py" ]]; then
    install -m 755 "${ROOT_DIR}/scripts/tinydvd-set-thumbnail.py" "${BIN_DIR}/tinydvd-set-thumbnail"
    ln -sf tinydvd-set-thumbnail "${BIN_DIR}/tinydvd-add-thumbnail"
fi

# 4. Install Desktop entry, MIME package, and Thumbnailer
echo "Installing desktop entry to ${APP_DIR}..."
# Set exact binary path in Exec and TryExec so GNOME desktop can always locate
# tinydvd even if ~/.local/bin is not in the GUI session's PATH.
sed -e "s|^Exec=.*|Exec=${BIN_DIR}/tinydvd -f %f|" \
    "${ROOT_DIR}/data/tinydvd.desktop" > "${APP_DIR}/tinydvd.desktop"
if ! grep -q "^TryExec=" "${APP_DIR}/tinydvd.desktop"; then
    sed -i -e "/^Exec=/i TryExec=${BIN_DIR}/tinydvd" "${APP_DIR}/tinydvd.desktop" 2>/dev/null || \
    sed -i "" -e "/^Exec=/i\\
TryExec=${BIN_DIR}/tinydvd
" "${APP_DIR}/tinydvd.desktop"
fi
chmod 644 "${APP_DIR}/tinydvd.desktop"

echo "Installing MIME package to ${MIME_DIR}..."
install -m 644 "${ROOT_DIR}/data/tinydvd.xml" "${MIME_DIR}/tinydvd.xml"

echo "Installing GNOME thumbnailer to ${THUMB_DIR}..."
sed -e "s|^Exec=.*|Exec=${BIN_DIR}/tinydvd-thumbnailer -s %s %i %o|" \
    -e "s|^TryExec=.*|TryExec=${BIN_DIR}/tinydvd-thumbnailer|" \
    "${ROOT_DIR}/data/tinydvd.thumbnailer" > "${THUMB_DIR}/tinydvd.thumbnailer"
chmod 644 "${THUMB_DIR}/tinydvd.thumbnailer"

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

# 7. Set default handler for .tdvd MIME types in mimeapps.list and xdg-mime
echo "Registering tinydvd as default player for .tdvd / .tinydvd..."

update_mimeapps_file() {
    local target="$1"
    mkdir -p "$(dirname "$target")"
    touch "$target"
    python3 - "$target" << 'PYEOF'
import sys, configparser, os

path = sys.argv[1]
cp = configparser.ConfigParser(interpolation=None)
cp.optionxform = str
if os.path.exists(path):
    try:
        cp.read(path)
    except Exception:
        pass

for section in ["Default Applications", "Added Associations"]:
    if not cp.has_section(section):
        cp.add_section(section)

mimes = ["video/x-tinydvd", "application/x-tdvd", "application/x-tinydvd"]

for m in mimes:
    cp.set("Default Applications", m, "tinydvd.desktop")
    curr = cp.get("Added Associations", m, fallback="")
    items = [x for x in curr.split(";") if x]
    if "tinydvd.desktop" not in items:
        items.insert(0, "tinydvd.desktop")
    cp.set("Added Associations", m, ";".join(items) + ";")

with open(path, "w") as f:
    cp.write(f)
PYEOF
}

# Update applications mimeapps.list
update_mimeapps_file "${APP_DIR}/mimeapps.list"

# Update user-specific mimeapps.list
if [[ "$USER_MODE" -eq 1 ]] || [[ "$PREFIX" == "${HOME}/.local" ]]; then
    update_mimeapps_file "${HOME}/.config/mimeapps.list"
fi

# If run via sudo, also configure the calling user's ~/.config/mimeapps.list and /etc/xdg
if [[ -n "${SUDO_USER:-}" ]] && [[ "$SUDO_USER" != "root" ]]; then
    USER_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6 || echo "")"
    if [[ -n "$USER_HOME" ]] && [[ -d "$USER_HOME" ]]; then
        update_mimeapps_file "${USER_HOME}/.config/mimeapps.list"
        chown "$SUDO_USER":"$(id -gn "$SUDO_USER")" "${USER_HOME}/.config/mimeapps.list" || true
    fi
    if [[ -d "/etc/xdg" ]]; then
        update_mimeapps_file "/etc/xdg/mimeapps.list" || true
    fi
fi

if command -v xdg-mime >/dev/null 2>&1; then
    xdg-mime default tinydvd.desktop video/x-tinydvd || true
    xdg-mime default tinydvd.desktop application/x-tdvd || true
    xdg-mime default tinydvd.desktop application/x-tinydvd || true
    if [[ -n "${SUDO_USER:-}" ]] && [[ "$SUDO_USER" != "root" ]]; then
        su - "$SUDO_USER" -c "xdg-mime default tinydvd.desktop video/x-tinydvd" 2>/dev/null || true
    fi
fi

echo ""
echo "Installation complete!"
echo "Files ending in .tdvd and .tinydvd are now mapped to tinydvd in GNOME / Nautilus."
echo "You can now double-click any .tdvd movie in GNOME Files to launch tinydvd."
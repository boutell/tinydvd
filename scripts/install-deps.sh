#!/usr/bin/env bash
# tinydvd dependency installer script
# Supports macOS (Homebrew) and Linux (Debian/Ubuntu, Fedora, Arch)
#
# Usage:
#   ./scripts/install-deps.sh [options]
#
# Options:
#   --with-libdvdcss     Opt in to installing optional dvdbackup/libdvdcss support
#   --without-libdvdcss  Skip optional CSS/optical-rip packages without prompting
#   -h, --help           Show this help text

set -euo pipefail

OPT_IN_LIBDVDCSS="ask"

prompt_opt_in_libdvdcss() {
    case "${OPT_IN_LIBDVDCSS}" in
        yes) return 0 ;;
        no)  return 1 ;;
    esac

    local cur_os="${OS:-$(uname -s)}"
    local prompt_msg=""
    if [[ -t 0 ]]; then
        echo ""
        if [[ "$cur_os" == "Darwin" ]]; then
            echo "Optional: Direct optical-drive ripping via dvdbackup."
            echo "On macOS, Homebrew's dvdbackup also installs libdvdread and libdvdcss"
            echo "(for CSS-encrypted discs, where permitted by local law)."
            echo "If you opt out, tinydvd still plays .tdvd files and imports previously ripped"
            echo "VIDEO_TS folders, unencrypted .iso images, and unencrypted discs mounted"
            echo "under /Volumes/<DISC>."
            prompt_msg="Install optional dvdbackup (includes libdvdread & libdvdcss via Homebrew)? [y/N]: "
        else
            echo "Optional: CSS-encrypted DVD decryption (libdvdcss, where permitted by local law)."
            echo "If you opt out, tinydvd still plays .tdvd files and imports unencrypted discs,"
            echo "previously ripped VIDEO_TS folders, and unencrypted .iso images (mounted first,"
            echo "or directly if 7z is installed)."
            prompt_msg="Install optional libdvdcss support for CSS-encrypted DVDs? [y/N]: "
        fi
        local reply=""
        read -r -p "$prompt_msg" reply || true
        case "$reply" in
            [yY]|[yY][eE][sS]) return 0 ;;
            *) return 1 ;;
        esac
    else
        if [[ "$cur_os" == "Darwin" ]]; then
            echo "Note: Non-interactive shell; skipping optional dvdbackup (and its Homebrew libdvdread/libdvdcss dependencies). You can still play .tdvd files and import VIDEO_TS folders, unencrypted .iso images, and mounted unencrypted discs under /Volumes/<DISC>. Pass --with-libdvdcss to opt in."
        else
            echo "Note: Non-interactive shell; skipping optional libdvdcss. You can still play .tdvd files and import unencrypted discs, previously ripped VIDEO_TS folders, and unencrypted .iso images (mounted or via 7z). Pass --with-libdvdcss to opt in."
        fi
        return 1
    fi
}

# Say why the package manager needs sudo before it asks for a password
explain_package_sudo() {
    if [ "$(id -u)" -ne 0 ]; then
        echo ""
        echo "=== Administrator rights needed ==="
        echo "The build tools and libraries TinyDVD needs are installed with your system"
        echo "package manager ($1), which needs administrator rights, so sudo may ask"
        echo "for your password. It installs a C compiler, CMake, pkg-config, ffmpeg, Python,"
        echo "the development files for mpv, SDL2, OpenGL, Cairo, Pango and zlib, and"
        echo "dvdbackup where your distribution has it. Optional DVD decryption (libdvdcss)"
        echo "is asked about separately. To install packages yourself instead, press Ctrl-C"
        echo "now (the README lists the exact commands)."
        echo ""
    fi
}

main() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --with-libdvdcss)
                OPT_IN_LIBDVDCSS="yes"
                shift
                ;;
            --without-libdvdcss)
                OPT_IN_LIBDVDCSS="no"
                shift
                ;;
            -h|--help)
                sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
                exit 0
                ;;
            *)
                echo "Unknown option: $1 (try --help)" >&2
                exit 1
                ;;
        esac
    done

    echo "=== tinydvd Dependency Installer ==="

    OS="$(uname -s)"
    case "$OS" in
        Darwin)
            echo "Detected operating system: macOS"
            if ! command -v brew >/dev/null 2>&1; then
                echo "Error: Homebrew is required on macOS. Install it from https://brew.sh/"
                exit 1
            fi
            echo "Installing core dependencies via Homebrew (pinning python@3.12 without altering default python3)..."
            brew install \
                cmake \
                pkg-config \
                mpv \
                sdl2 \
                cairo \
                pango \
                ffmpeg \
                python@3.12
            if prompt_opt_in_libdvdcss; then
                brew install dvdbackup
            fi
            ;;
        Linux)
            echo "Detected operating system: Linux"
            if [ -f /etc/debian_version ]; then
                echo "Detected Debian/Ubuntu-based distribution."
                echo "Refreshing apt package lists and installing packages (may require sudo)..."
                SUDO=""
                if [ "$(id -u)" -ne 0 ]; then
                    SUDO="sudo"
                fi
                explain_package_sudo apt-get
                $SUDO apt-get update
                $SUDO apt-get install -y \
                    build-essential \
                    cmake \
                    pkg-config \
                    dvdbackup \
                    libmpv-dev \
                    libsdl2-dev \
                    libgl1-mesa-dev \
                    libcairo2-dev \
                    libpango1.0-dev \
                    zlib1g-dev \
                    ffmpeg \
                    python3 \
                    python3-venv
                if prompt_opt_in_libdvdcss; then
                    if $SUDO apt-get install -y libdvd-pkg 2>/dev/null; then
                        echo "Configuring libdvd-pkg to build and install libdvdcss..."
                        $SUDO dpkg-reconfigure -f noninteractive libdvd-pkg || true
                    else
                        echo "Note: 'libdvd-pkg' not found (requires Debian 'contrib' or Ubuntu 'multiverse' repo); skipping optional CSS package."
                    fi
                fi
            elif [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
                echo "Detected Fedora/RHEL-based distribution."
                SUDO=""
                if [ "$(id -u)" -ne 0 ]; then
                    SUDO="sudo"
                fi
                explain_package_sudo dnf
                $SUDO dnf install -y \
                    gcc \
                    gcc-c++ \
                    cmake \
                    pkgconf-pkg-config \
                    mpv-libs-devel \
                    SDL2-devel \
                    mesa-libGL-devel \
                    cairo-devel \
                    pango-devel \
                    zlib-devel \
                    ffmpeg \
                    python3
                if prompt_opt_in_libdvdcss; then
                    echo "Note: Fedora/RHEL ships optical disc ripping tools via RPM Fusion:"
                    echo "  - Without CSS encryption (unencrypted optical drives via RPM Fusion Free):"
                    echo "      sudo dnf install -y dvdbackup"
                    echo "  - With optional CSS decryption (via RPM Fusion Tainted):"
                    echo "      sudo dnf install -y rpmfusion-free-release-tainted && sudo dnf install -y libdvdcss"
                fi
            elif [ -f /etc/arch-release ]; then
                echo "Detected Arch Linux."
                SUDO=""
                if [ "$(id -u)" -ne 0 ]; then
                    SUDO="sudo"
                fi
                explain_package_sudo pacman
                $SUDO pacman -S --needed --noconfirm \
                    base-devel \
                    cmake \
                    pkgconf \
                    dvdbackup \
                    mpv \
                    sdl2 \
                    cairo \
                    pango \
                    zlib \
                    ffmpeg \
                    python
                if prompt_opt_in_libdvdcss; then
                    $SUDO pacman -S --needed --noconfirm libdvdcss
                fi
            else
                echo "Unknown Linux distribution. Please manually install:"
                echo "  - cmake, pkg-config, C11 compiler, zlib dev headers"
                echo "  - libmpv (mpv dev headers and library)"
                echo "  - SDL2, cairo, pango (development headers and libraries)"
                echo "  - ffmpeg with libx264 support"
                echo "  - python3 (>= 3.8)"
                echo "  - Optional (for physical optical drive ripping): dvdbackup (and optionally libdvdcss)"
                exit 1
            fi
            ;;
        *)
            echo "Unsupported operating system: $OS"
            exit 1
            ;;
    esac

    echo ""
    echo "=== Verifying Installed Dependencies ==="
    if [ "$OS" = "Darwin" ]; then
        export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
    fi
    pkg-config --modversion mpv || echo "Warning: mpv pkg-config not found"
    pkg-config --modversion sdl2 || echo "Warning: sdl2 pkg-config not found"
    pkg-config --modversion cairo || echo "Warning: cairo pkg-config not found"
    pkg-config --modversion pangocairo || echo "Warning: pangocairo pkg-config not found"
    if command -v dvdbackup >/dev/null 2>&1; then
        echo "dvdbackup (optional): $(dvdbackup --version 2>&1 | head -n 1)"
    else
        echo "Optional dvdbackup not installed (not needed for .tdvd playback, VIDEO_TS folders, unencrypted ISOs, or mounted unencrypted discs)."
    fi
    ffmpeg -version | head -n 1 || echo "Warning: ffmpeg not found"
    cmake --version | head -n 1 || echo "Warning: cmake not found"

    PINNED_PYTHON=""
    if [ "$OS" = "Darwin" ]; then
        PY312_PREFIX="$(brew --prefix python@3.12 2>/dev/null || true)"
        if [ -n "$PY312_PREFIX" ] && [ -x "${PY312_PREFIX}/bin/python3.12" ]; then
            PINNED_PYTHON="${PY312_PREFIX}/bin/python3.12"
        fi
    elif [ -x /usr/bin/python3 ]; then
        PINNED_PYTHON="/usr/bin/python3"
    fi
    if [ -z "$PINNED_PYTHON" ] && command -v python3 >/dev/null 2>&1; then
        PINNED_PYTHON="$(command -v python3)"
    fi

    if [ -n "$PINNED_PYTHON" ]; then
        HOST_ARCH="$(uname -m)"
        echo "Pinned Python: $PINNED_PYTHON ($("$PINNED_PYTHON" --version 2>&1))"
        "$PINNED_PYTHON" - "$HOST_ARCH" <<'PYEOF'
import ctypes, http.server, platform, select, sys, urllib.request, zipfile, zlib

host_arch = sys.argv[1]
if sys.version_info < (3, 8):
    raise SystemExit(f"Error: Python >= 3.8 required, found {sys.version.split()[0]}")
py_arch = platform.machine()
if host_arch and py_arch and py_arch != host_arch:
    raise SystemExit(f"Error: Python architecture {py_arch} does not match host {host_arch}")
print(f"Verified Python {sys.version.split()[0]} ({py_arch}) with required stdlib modules.")
PYEOF
    else
        echo "Warning: compatible python3 not found"
    fi

    echo ""
    echo "Dependencies installed successfully!"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi

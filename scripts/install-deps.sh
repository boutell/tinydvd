#!/usr/bin/env bash
# tinydvd dependency installer script
# Supports macOS (Homebrew) and Linux (Debian/Ubuntu, Fedora, Arch)

set -euo pipefail

echo "=== tinydvd Dependency Installer ==="

OS="$(uname -s)"
case "$OS" in
    Darwin)
        echo "Detected operating system: macOS"
        if ! command -v brew >/dev/null 2>&1; then
            echo "Error: Homebrew is required on macOS. Install it from https://brew.sh/"
            exit 1
        fi
        echo "Installing dependencies via Homebrew (pinning python@3.12 without altering default python3)..."
        brew install \
            cmake \
            pkg-config \
            libdvdnav \
            libdvdread \
            libdvdcss \
            dvdbackup \
            mpv \
            sdl2 \
            cairo \
            pango \
            ffmpeg \
            python@3.12
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
            $SUDO apt-get update
            $SUDO apt-get install -y \
                build-essential \
                cmake \
                pkg-config \
                libdvdnav-dev \
                libdvdread-dev \
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
            if $SUDO apt-get install -y libdvd-pkg 2>/dev/null; then
                echo "Configuring libdvd-pkg to build and install libdvdcss..."
                $SUDO dpkg-reconfigure -f noninteractive libdvd-pkg || true
            else
                echo "Note: 'libdvd-pkg' not found (requires Debian 'contrib' repo); skipping optional CSS package."
            fi
        elif [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
            echo "Detected Fedora/RHEL-based distribution."
            SUDO=""
            if [ "$(id -u)" -ne 0 ]; then
                SUDO="sudo"
            fi
            $SUDO dnf install -y \
                gcc \
                gcc-c++ \
                cmake \
                pkgconf-pkg-config \
                libdvdnav-devel \
                libdvdread-devel \
                mpv-libs-devel \
                SDL2-devel \
                mesa-libGL-devel \
                cairo-devel \
                pango-devel \
                zlib-devel \
                ffmpeg \
                python3
        elif [ -f /etc/arch-release ]; then
            echo "Detected Arch Linux."
            SUDO=""
            if [ "$(id -u)" -ne 0 ]; then
                SUDO="sudo"
            fi
            $SUDO pacman -S --needed --noconfirm \
                base-devel \
                cmake \
                pkgconf \
                libdvdnav \
                libdvdread \
                mpv \
                sdl2 \
                cairo \
                pango \
                zlib \
                ffmpeg \
                python
        else
            echo "Unknown Linux distribution. Please manually install:"
            echo "  - cmake, pkg-config, C11 compiler"
            echo "  - libdvdnav, libdvdread"
            echo "  - libmpv (mpv dev headers and library)"
            echo "  - SDL2, cairo, pango (development headers and libraries)"
            echo "  - ffmpeg with libx264 support"
            echo "  - python3 (>= 3.8)"
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
pkg-config --modversion dvdnav || echo "Warning: dvdnav pkg-config not found"
pkg-config --modversion dvdread || echo "Warning: dvdread pkg-config not found"
pkg-config --modversion mpv || echo "Warning: mpv pkg-config not found"
pkg-config --modversion sdl2 || echo "Warning: sdl2 pkg-config not found"
pkg-config --modversion cairo || echo "Warning: cairo pkg-config not found"
pkg-config --modversion pangocairo || echo "Warning: pangocairo pkg-config not found"
command -v dvdbackup >/dev/null 2>&1 && echo "dvdbackup: $(dvdbackup --version 2>&1 | head -n 1)" || echo "Warning: dvdbackup not found"
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

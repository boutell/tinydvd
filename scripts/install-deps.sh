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
        echo "Installing dependencies via Homebrew..."
        brew install \
            cmake \
            pkg-config \
            libdvdnav \
            libdvdread \
            libdvdcss \
            dvdbackup \
            mpv \
            sdl2 \
            ffmpeg \
            python3
        ;;
    Linux)
        echo "Detected operating system: Linux"
        if [ -f /etc/debian_version ]; then
            echo "Detected Debian/Ubuntu-based distribution."
            echo "Running apt-get update and installing packages (may require sudo)..."
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
                libdvd-pkg \
                dvdbackup \
                libmpv-dev \
                libsdl2-dev \
                libgl1-mesa-dev \
                ffmpeg \
                python3 \
                python3-pip
            echo "Configuring libdvd-pkg to build and install libdvdcss..."
            $SUDO dpkg-reconfigure -f noninteractive libdvd-pkg || true
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
                ffmpeg \
                python
        else
            echo "Unknown Linux distribution. Please manually install:"
            echo "  - cmake, pkg-config, C11 compiler"
            echo "  - libdvdnav, libdvdread"
            echo "  - libmpv (mpv dev headers and library)"
            echo "  - SDL2 (development headers and library)"
            echo "  - ffmpeg with libx264 support"
            echo "  - python3"
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
pkg-config --modversion dvdnav || echo "Warning: dvdnav pkg-config not found"
pkg-config --modversion dvdread || echo "Warning: dvdread pkg-config not found"
pkg-config --modversion mpv || echo "Warning: mpv pkg-config not found"
pkg-config --modversion sdl2 || echo "Warning: sdl2 pkg-config not found"
command -v dvdbackup >/dev/null 2>&1 && echo "dvdbackup: $(dvdbackup --version 2>&1 | head -n 1)" || echo "Warning: dvdbackup not found"
ffmpeg -version | head -n 1 || echo "Warning: ffmpeg not found"
python3 --version || echo "Warning: python3 not found"
cmake --version | head -n 1 || echo "Warning: cmake not found"

echo ""
echo "Dependencies installed successfully!"

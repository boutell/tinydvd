#!/usr/bin/env bash
# tinydvd dependency installer script
# Supports macOS (Homebrew) and Linux (Debian/Ubuntu, Fedora, Arch)
#
# Usage:
#   ./scripts/install-deps.sh [options]
#
# Options:
#   --with-libdvdcss     Install optional encrypted-DVD support (dvdbackup + libdvdcss)
#                        without asking
#   --without-libdvdcss  Skip it without asking
#   --only-libdvdcss     Only offer/install that optional support (used by ./install)
#   -h, --help           Show this help text

set -euo pipefail

OPT_IN_LIBDVDCSS="ask"
ONLY_LIBDVDCSS=0
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

# Which package family this system uses: macos, debian, fedora, arch or unknown
detect_platform() {
    if [[ "$(uname -s)" == "Darwin" ]]; then
        echo macos
    elif [ -f /etc/debian_version ]; then
        echo debian
    elif [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
        echo fedora
    elif [ -f /etc/arch-release ]; then
        echo arch
    else
        echo unknown
    fi
}

have_libdvdcss() {
    local ldconfig
    for ldconfig in ldconfig /sbin/ldconfig /usr/sbin/ldconfig; do
        if command -v "$ldconfig" >/dev/null 2>&1; then
            "$ldconfig" -p 2>/dev/null | grep -q 'libdvdcss\.so\.2' && return 0
            break
        fi
    done
    compgen -G '/usr/lib*/libdvdcss.so.2' >/dev/null || compgen -G '/usr/lib*/*/libdvdcss.so.2' >/dev/null
}

# True if ripping encrypted discs already works: dvdbackup plus libdvdcss
# (on macOS, Homebrew's dvdbackup always brings libdvdcss)
have_disc_support() {
    command -v dvdbackup >/dev/null 2>&1 || return 1
    [[ "$(detect_platform)" == "macos" ]] && return 0
    have_libdvdcss
}

prompt_opt_in_libdvdcss() {
    case "${OPT_IN_LIBDVDCSS}" in
        yes) return 0 ;;
        no)  return 1 ;;
    esac
    if [[ ! -t 0 ]]; then
        echo "Note: no terminal to ask in; skipping optional encrypted-DVD support. Pass --with-libdvdcss to include it."
        return 1
    fi
    echo ""
    echo "=== Optional: rip DVDs straight from a drive, including encrypted ones ==="
    echo "Most commercial DVDs are CSS-encrypted. Ripping them needs dvdbackup and"
    echo "libdvdcss, the standard open-source decryption library (VLC uses it too)."
    echo "Whether you may use it depends on where you live; check your local law."
    echo "Without it, TinyDVD still plays .tdvd files and imports ripped VIDEO_TS"
    echo "folders, unencrypted .iso images and unencrypted discs."
    local reply=""
    read -r -p "Install dvdbackup and libdvdcss? [y/N]: " reply || true
    case "$reply" in
        [yY]|[yY][eE][sS]) return 0 ;;
        *) return 1 ;;
    esac
}

# Offer, and if accepted install, dvdbackup + libdvdcss from the platform's
# usual packages. Returns nonzero only if an accepted install failed.
install_disc_support() {
    local platform
    platform="$(detect_platform)"
    if have_disc_support; then
        echo "Encrypted-DVD support (dvdbackup + libdvdcss): already installed."
        return 0
    fi
    if ! prompt_opt_in_libdvdcss; then
        echo "Skipped encrypted-DVD support. To add it later: ./install --with-libdvdcss"
        return 0
    fi
    case "$platform" in
        macos)
            # Homebrew's dvdbackup depends on libdvdread, which brings libdvdcss
            brew install dvdbackup
            ;;
        debian)
            # libdvd-pkg downloads the libdvdcss source and builds it for you
            if [ -n "$SUDO" ]; then
                echo "sudo may ask for your password to install dvdbackup and libdvd-pkg."
            fi
            $SUDO apt-get install -y dvdbackup
            if ! apt-cache show libdvd-pkg >/dev/null 2>&1; then
                echo "libdvd-pkg isn't available: it's in Debian's 'contrib' section and"
                echo "Ubuntu's 'multiverse' repository. Enable that, then run ./install --with-libdvdcss"
                echo "  Ubuntu: sudo add-apt-repository multiverse && sudo apt-get update"
                echo "  Debian: add 'contrib' to the components in /etc/apt/sources.list(.d), then sudo apt-get update"
                return 1
            fi
            $SUDO apt-get install -y libdvd-pkg
            # This step fetches and builds libdvdcss
            $SUDO dpkg-reconfigure -f noninteractive libdvd-pkg
            ;;
        fedora)
            # Fedora can't ship libdvdcss; RPM Fusion's "tainted" repository does
            if ! rpm -q rpmfusion-free-release >/dev/null 2>&1; then
                echo "dvdbackup and libdvdcss come from RPM Fusion, which isn't enabled yet."
                echo "Enable RPM Fusion Free (https://rpmfusion.org/Configuration), then run"
                echo "./install --with-libdvdcss"
                return 1
            fi
            if [ -n "$SUDO" ]; then
                echo "sudo may ask for your password to enable RPM Fusion tainted and install the packages."
            fi
            $SUDO dnf install -y rpmfusion-free-release-tainted
            $SUDO dnf install -y dvdbackup libdvdcss
            ;;
        arch)
            if [ -n "$SUDO" ]; then
                echo "sudo may ask for your password to install dvdbackup and libdvdcss."
            fi
            $SUDO pacman -S --needed --noconfirm dvdbackup libdvdcss
            ;;
        *)
            echo "Install dvdbackup and libdvdcss with your distribution's package manager."
            return 1
            ;;
    esac
    if have_disc_support; then
        echo "Encrypted-DVD support installed."
    else
        echo "Warning: dvdbackup or libdvdcss still not found after installing." >&2
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
            --only-libdvdcss)
                ONLY_LIBDVDCSS=1
                shift
                ;;
            -h|--help)
                sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
                exit 0
                ;;
            *)
                echo "Unknown option: $1 (try --help)" >&2
                exit 1
                ;;
        esac
    done

    if [[ "$ONLY_LIBDVDCSS" -eq 1 ]]; then
        install_disc_support
        return
    fi

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
            ;;
        Linux)
            echo "Detected operating system: Linux"
            if [ -f /etc/debian_version ]; then
                echo "Detected Debian/Ubuntu-based distribution."
                echo "Refreshing apt package lists and installing packages (may require sudo)..."
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
            elif [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
                echo "Detected Fedora/RHEL-based distribution."
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
            elif [ -f /etc/arch-release ]; then
                echo "Detected Arch Linux."
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

    install_disc_support || echo "Note: encrypted-DVD support wasn't installed; everything else is unaffected."

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

#!/usr/bin/env python3
"""
Unified Python CLI entrypoint for TinyDVD Player & Library Server.

Orchestrates TinyDVDDesktopUI, TinyDVDWebServer, and libtinydvd_core's media &
canvas primitives.
"""

from __future__ import annotations

import os
import signal
import sys

# Ensure sibling modules in scripts/ are importable regardless of cwd
SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, SCRIPTS_DIR)

from tinydvd_core import TinyDVDCore
from tinydvd_desktop_ui import run_desktop_app
from tinydvd_web_server import TinyDVDWebServer


TINYDVD_VERSION = "1.0.0"


def resolve_default_library_path() -> str:
    home = os.path.expanduser("~")
    if not home or home == "~":
        return "."
    movies = os.path.join(home, "Movies")
    videos = os.path.join(home, "Videos")
    if os.path.isdir(movies):
        return movies
    if os.path.isdir(videos):
        return videos
    return movies if sys.platform == "darwin" else videos


def print_usage(prog: str) -> None:
    print(f"tinydvd Player v{TINYDVD_VERSION}")
    print("Interactive DVD-Video Hybrid Player (.tdvd / H.264 + Meta-VOB)\n")
    print(f"Usage: {prog} [options] [movie.tdvd | library_directory]\n")
    print("Options:")
    print("  -s, --serve          Start embedded web streaming server")
    print("  -p, --port <port>    Web server port (default: 8080)")
    print("  -b, --bind <ip>      Web server bind address (default: 0.0.0.0)")
    print("  -f, --fullscreen     Start in fullscreen mode")
    print("  -h, --help           Show this help text")
    print("  -v, --version        Show version information\n")
    print("Controls:")
    print("  Arrow Keys (in menu) Navigate menu buttons")
    print("  Enter / Space        Activate selected menu button")
    print("  Mouse Click / Move   Direct DVD menu button selection")
    print("  Left / Right Arrow   Seek -10s / +10s along clip timeline")
    print("  Shift + Left / Right Seek -60s / +60s along clip timeline")
    print("  Down / Up Arrow      Seek -60s / +60s along clip timeline")
    print("  , / . (Comma/Period) Seek -10s / +10s along clip timeline")
    print("  [ / ] or PgUp / PgDn Previous / Next Chapter")
    print("  0 - 9                Jump to 0% - 90% of clip timeline")
    print("  C / S                Cycle Closed Captions / Subtitles (Track 1..N, Off)")
    print("  A                    Cycle Audio tracks")
    print("  Space (in film)      Toggle pause")
    print("  M                    Jump to DVD Root Menu")
    print("  Escape               Go up a menu level (or return to library / exit)")
    print("  F                    Toggle fullscreen")
    print("  Q                    Quit")


def main(argv: list[str]) -> int:
    prog = os.environ.get("TINYDVD_PROG") or argv[0]
    dvd_path = None
    start_fullscreen = False
    serve_mode = False
    port = 8080
    bind_addr = "0.0.0.0"

    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg in ("-h", "--help"):
            print_usage(prog)
            return 0
        if arg in ("-v", "--version"):
            print(f"tinydvd v{TINYDVD_VERSION}")
            return 0
        if arg in ("-s", "--serve"):
            serve_mode = True
        elif arg in ("-p", "--port"):
            if i + 1 < len(argv):
                i += 1
                try:
                    port = int(argv[i])
                except ValueError:
                    print(f"Invalid port: {argv[i]}", file=sys.stderr)
                    return 1
        elif arg in ("-b", "--bind"):
            if i + 1 < len(argv):
                i += 1
                bind_addr = argv[i]
        elif arg in ("-f", "--fullscreen"):
            start_fullscreen = True
        elif not arg.startswith("-"):
            dvd_path = arg
        else:
            print(f"Unknown option: {arg}", file=sys.stderr)
            print_usage(prog)
            return 1
        i += 1

    if not dvd_path:
        if serve_mode:
            print("Error: No DVD path specified.\n", file=sys.stderr)
            print_usage(prog)
            return 1
        dvd_path = resolve_default_library_path()

    core = TinyDVDCore()

    if serve_mode:
        server = TinyDVDWebServer(core, dvd_path, bind_addr=bind_addr, port=port)

        def on_sig(_signum, _frame):
            server.running = False

        signal.signal(signal.SIGINT, on_sig)
        signal.signal(signal.SIGTERM, on_sig)
        server.serve_until_interrupted()
        print("\nStopping web server...")
        server.stop()
        return 0

    return run_desktop_app(core, dvd_path, start_fullscreen)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

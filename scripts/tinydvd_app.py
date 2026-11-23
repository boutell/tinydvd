#!/usr/bin/env python3
"""
Unified Python CLI entrypoint for TinyDVD Player & Library Server.

Orchestrates TinyDVDDesktopUI, TinyDVDWebServer, and libtinydvd_core's media &
canvas primitives.
"""

from __future__ import annotations

import json
import os
import secrets
import signal
import sys

# Ensure sibling modules in scripts/ are importable regardless of cwd
SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, SCRIPTS_DIR)

from tinydvd_core import TinyDVDCore
from tinydvd_desktop_ui import PLAYER_KEY_HELP, run_desktop_app
from tinydvd_library import default_library_path, detect_default_source, preview_import_metadata
from tinydvd_web_server import TinyDVDWebServer


TINYDVD_VERSION = "1.0.0"


def config_dir() -> str:
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/tinydvd")
    return os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "tinydvd")


def load_web_password(password_file: str | None) -> tuple[str, str]:
    """
    The web login password and where it came from: TINYDVD_WEB_PASSWORD,
    --web-password-file, or a random one saved (readable only by you) in the
    config folder on first use.
    """
    env = os.environ.get("TINYDVD_WEB_PASSWORD", "")
    if env:
        return env, "the TINYDVD_WEB_PASSWORD environment variable"
    path = password_file or os.path.join(config_dir(), "web-password")
    try:
        with open(path, "r", encoding="utf-8") as f:
            pw = f.read().strip()
        if pw:
            return pw, path
    except FileNotFoundError:
        if password_file:
            raise
    pw = secrets.token_urlsafe(12)
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(pw + "\n")
    return pw, path


def print_usage(prog: str) -> None:
    print(f"tinydvd Player v{TINYDVD_VERSION}")
    print("Interactive DVD-Video Hybrid Player (.tdvd / H.264 + Meta-VOB)\n")
    print(f"Usage: {prog} [options] [movie.tdvd | library_directory]\n")
    print(f"With no path, it uses your videos folder ({default_library_path()}).\n")
    print("Options:")
    print("  -s, --serve               Start the web player server")
    print("  -p, --port <port>         Web server port (default: 8080)")
    print("  -b, --bind <ip>           Web server address (default: 127.0.0.1, this computer only;")
    print("                            use 0.0.0.0 to allow other devices on your network)")
    print("  --web-password-file <f>   Read the web login password from this file")
    print("                            (default: TINYDVD_WEB_PASSWORD, or a random password")
    print("                            saved in your config folder on first run)")
    print("  --allow-web-import        Let web users import DVDs (runs the ripping and")
    print("                            conversion tools on paths they type)")
    print("  --lookup [source]         Show the movie details an import of a disc would store,")
    print("                            without importing (a drive like /dev/sr0, a mounted disc,")
    print("                            a VIDEO_TS folder, or a .tdvd; default: the disc in the drive)")
    print("  --title <title>           With --lookup: the title typed in the import dialog")
    print("  -f, --fullscreen          Start in fullscreen mode")
    print("  -h, --help                Show this help text")
    print("  -v, --version             Show version information\n")
    print("Player keys:")
    width = max(len(keys) for keys, _ in PLAYER_KEY_HELP) + 2
    for keys, desc in PLAYER_KEY_HELP:
        print(f"  {keys.ljust(width)}{desc}")


def run_lookup(src: str | None, title: str) -> int:
    """
    Look up a disc's movie details exactly as the import's "Fetch movie
    metadata" option does, and print what would go in its metadata.json
    """
    src = src or detect_default_source()[0]
    if not src:
        print("No disc found; name one (e.g. /dev/sr0).", file=sys.stderr)
        return 1
    try:
        meta = preview_import_metadata(src, title)
    except ValueError as e:
        print(e, file=sys.stderr)
        return 1
    print("\nmetadata.json:")
    print(json.dumps(meta, indent=2, ensure_ascii=False))
    return 0


def main(argv: list[str]) -> int:
    prog = os.environ.get("TINYDVD_PROG") or argv[0]
    dvd_path = None
    start_fullscreen = False
    serve_mode = False
    port = 8080
    bind_addr = "127.0.0.1"
    password_file = None
    allow_import = False
    lookup = False
    title = ""

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
        elif arg == "--web-password-file":
            if i + 1 < len(argv):
                i += 1
                password_file = argv[i]
        elif arg == "--lookup":
            lookup = True
        elif arg == "--title":
            if i + 1 < len(argv):
                i += 1
                title = argv[i]
        elif arg == "--allow-web-import":
            allow_import = True
        elif arg in ("-f", "--fullscreen"):
            start_fullscreen = True
        elif not arg.startswith("-"):
            dvd_path = arg
        else:
            print(f"Unknown option: {arg}", file=sys.stderr)
            print_usage(prog)
            return 1
        i += 1

    if lookup:
        return run_lookup(dvd_path, title)

    if not dvd_path:
        dvd_path = default_library_path()

    core = TinyDVDCore()

    if serve_mode:
        try:
            password, password_source = load_web_password(password_file)
        except OSError as e:
            print(f"Can't read the web password: {e}", file=sys.stderr)
            return 1
        server = TinyDVDWebServer(core, dvd_path, password, bind_addr=bind_addr, port=port,
                                  allow_import=allow_import)
        if sys.stdout.isatty():
            print(f"  Password: {password}  (from {password_source})\n", flush=True)
        else:
            # A log (such as the systemd journal) may be readable by others
            print(f"  Password: see {password_source}\n", flush=True)

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

#!/usr/bin/env python3
"""
Unified Python CLI entrypoint for TinyDVD Player & Library Server.

Orchestrates TinyDVDLibrary, TinyDVDWebServer, and libtinydvd_core's SDL2/OpenGL
desktop UI + mpv/libdvdread DVD VM playback engine.
"""

import ctypes
import os
import signal
import sys
import threading

# Ensure sibling modules in scripts/ are importable regardless of cwd
SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, SCRIPTS_DIR)

from tinydvd_core import (
    PY_DETECT_SOURCE_CB,
    PY_IMPORT_CANCEL_CB,
    PY_IMPORT_START_CB,
    PY_READ_THUMB_CB,
    PY_SET_DIR_CB,
    TinyDVDCore,
    write_c_char_buf,
)
from tinydvd_library import TinyDVDLibrary, detect_default_source
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


def run_desktop(core: TinyDVDCore, dvd_path: str, start_fullscreen: bool) -> int:
    sync_lock = threading.Lock()
    thumb_state = {"buf": None}

    def sync_catalog() -> None:
        is_dir, items, raw_target = lib.get_snapshot()
        gen = lib.get_generation()
        with sync_lock:
            core.lib.tdvd_desktop_set_library_mode(
                raw_target.encode("utf-8"), 1 if is_dir else 0
            )
            core.lib.tdvd_desktop_sync_catalog_begin()
            for it in items:
                core.lib.tdvd_desktop_sync_catalog_add(
                    it.id.encode("utf-8"),
                    it.title.encode("utf-8"),
                    it.path.encode("utf-8"),
                    it.size_bytes,
                    it.size_str.encode("utf-8"),
                    1 if it.has_thumbnail else 0,
                    it.year,
                    it.director.encode("utf-8"),
                    it.cast.encode("utf-8"),
                    it.genres.encode("utf-8"),
                    it.overview.encode("utf-8"),
                )
            core.lib.tdvd_desktop_sync_catalog_end(gen)

    def sync_import() -> None:
        st = lib.get_import_status()
        logs_joined = "\n".join(st["logs"]).encode("utf-8")
        core.lib.tdvd_desktop_sync_import_status(
            st["state_code"],
            1 if st["active"] else 0,
            st["start_time"],
            st["stage_msg"].encode("utf-8"),
            st["error_msg"].encode("utf-8"),
            st["result_disc_id"].encode("utf-8"),
            st["result_disc_path"].encode("utf-8"),
            st["result_title"].encode("utf-8"),
            logs_joined,
        )

    lib = TinyDVDLibrary(dvd_path, on_catalog_change=sync_catalog, on_import_change=sync_import)

    def cb_read_thumb(disc_path_cstr: bytes, out_ptr, out_sz) -> int:
        dpath = disc_path_cstr.decode("utf-8", errors="replace") if disc_path_cstr else ""
        data = lib.read_thumbnail(dpath)
        if not data:
            return 0
        cbuf = ctypes.create_string_buffer(data, len(data))
        thumb_state["buf"] = cbuf
        out_ptr[0] = ctypes.cast(cbuf, ctypes.c_void_p).value
        out_sz[0] = len(data)
        return 1

    def cb_detect_source(out_src: int, src_len: int, out_label: int, label_len: int) -> None:
        src, label = detect_default_source()
        write_c_char_buf(out_src, src_len, src)
        write_c_char_buf(out_label, label_len, label)

    def cb_import_start(
        source_cstr: bytes, title_cstr: bytes, skip_enrich: int, err_buf: int, err_len: int
    ) -> int:
        src = source_cstr.decode("utf-8", errors="replace") if source_cstr else ""
        title = title_cstr.decode("utf-8", errors="replace") if title_cstr else ""
        ok, err = lib.start_import(src, title=title, skip_enrichment=bool(skip_enrich))
        if not ok:
            write_c_char_buf(err_buf, err_len, err)
            return 0
        return 1

    def cb_import_cancel() -> None:
        lib.cancel_import()

    def cb_set_dir(dir_cstr: bytes) -> None:
        dpath = dir_cstr.decode("utf-8", errors="replace") if dir_cstr else ""
        if not dpath:
            return
        with lib.catalog_lock:
            same_lib = lib.is_library and (lib.target_path == lib._expand_path(dpath))
        if same_lib:
            lib.rescan()
        else:
            lib.set_directory(dpath)
        sync_catalog()

    # Keep strong references to ctypes callbacks for the entire C run lifetime
    cb_refs = (
        PY_READ_THUMB_CB(cb_read_thumb),
        PY_DETECT_SOURCE_CB(cb_detect_source),
        PY_IMPORT_START_CB(cb_import_start),
        PY_IMPORT_CANCEL_CB(cb_import_cancel),
        PY_SET_DIR_CB(cb_set_dir),
    )
    core.lib.tdvd_library_bind_python_callbacks(*cb_refs)

    sync_catalog()
    sync_import()

    try:
        return int(
            core.lib.tdvd_desktop_run(
                dvd_path.encode("utf-8"), 1 if start_fullscreen else 0
            )
        )
    finally:
        lib.destroy()
        thumb_state["buf"] = None
        del cb_refs


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

    return run_desktop(core, dvd_path, start_fullscreen)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

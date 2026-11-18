#!/usr/bin/env python3
"""
ctypes bindings to the C media & rendering engine (libtinydvd_core).

Keeps libdvdread IFO/NV_PCK VM execution, SPU RLE compositing, libmpv rendering,
and SDL2+OpenGL+Cairo native UI rendering in C while moving application logic,
library management, .tdvd archive parsing, OS directory watching, and HTTP/SSE/WS
serving to Python.
"""

import ctypes
import os
import sys


def find_core_library() -> str:
    env_path = os.environ.get("TINYDVD_CORE_LIB", "").strip()
    if env_path and os.path.isfile(env_path):
        return env_path

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ext = ".dylib" if sys.platform == "darwin" else ".so"
    candidates = [
        os.path.join(repo_root, "build", f"libtinydvd_core{ext}"),
        os.path.join(os.getcwd(), "build", f"libtinydvd_core{ext}"),
        os.path.join(os.path.dirname(sys.executable), f"libtinydvd_core{ext}"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    raise FileNotFoundError(f"Could not locate libtinydvd_core{ext} in build/ or TINYDVD_CORE_LIB")


PY_READ_THUMB_CB = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_void_p),
    ctypes.POINTER(ctypes.c_size_t),
)

PY_DETECT_SOURCE_CB = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.c_void_p,
    ctypes.c_size_t,
)

PY_IMPORT_START_CB = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_size_t,
)

PY_IMPORT_CANCEL_CB = ctypes.CFUNCTYPE(None)
PY_SET_DIR_CB = ctypes.CFUNCTYPE(None, ctypes.c_char_p)
TDVD_VM_JSON_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p)


def write_c_char_buf(addr: int, max_len: int, text: str) -> None:
    if not addr or max_len <= 0:
        return
    raw = (text or "").encode("utf-8", errors="replace")[: max_len - 1]
    if raw:
        ctypes.memmove(addr, raw, len(raw))
    ctypes.memset(addr + len(raw), 0, 1)


class TinyDVDCore:
    def __init__(self, lib_path: str | None = None):
        self.path = lib_path or find_core_library()
        self.lib = ctypes.CDLL(self.path)
        self._bind()

    def _bind(self) -> None:
        lib = self.lib

        lib.tdvd_free.argtypes = [ctypes.c_void_p]
        lib.tdvd_free.restype = None

        lib.tdvd_library_bind_python_callbacks.argtypes = [
            PY_READ_THUMB_CB,
            PY_DETECT_SOURCE_CB,
            PY_IMPORT_START_CB,
            PY_IMPORT_CANCEL_CB,
            PY_SET_DIR_CB,
        ]
        lib.tdvd_library_bind_python_callbacks.restype = None

        lib.tdvd_desktop_set_library_mode.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.tdvd_desktop_set_library_mode.restype = None

        lib.tdvd_desktop_sync_catalog_begin.argtypes = []
        lib.tdvd_desktop_sync_catalog_begin.restype = None

        lib.tdvd_desktop_sync_catalog_add.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
        ]
        lib.tdvd_desktop_sync_catalog_add.restype = None

        lib.tdvd_desktop_sync_catalog_end.argtypes = [ctypes.c_uint64]
        lib.tdvd_desktop_sync_catalog_end.restype = None

        lib.tdvd_desktop_sync_import_status.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int64,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
        ]
        lib.tdvd_desktop_sync_import_status.restype = None

        lib.tdvd_desktop_run.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.tdvd_desktop_run.restype = ctypes.c_int

        lib.tdvd_vm_session_create.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            TDVD_VM_JSON_CB,
            ctypes.c_void_p,
        ]
        lib.tdvd_vm_session_create.restype = ctypes.c_void_p

        lib.tdvd_vm_session_destroy.argtypes = [ctypes.c_void_p]
        lib.tdvd_vm_session_destroy.restype = None

        lib.tdvd_vm_session_load.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
        ]
        lib.tdvd_vm_session_load.restype = ctypes.c_int

        lib.tdvd_vm_session_dispatch_action.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_int,
        ]
        lib.tdvd_vm_session_dispatch_action.restype = None

        lib.tdvd_vm_session_set_paused.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.tdvd_vm_session_set_paused.restype = None

        lib.tdvd_vm_session_notify_playback.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_uint64,
            ctypes.c_int64,
            ctypes.c_int,
        ]
        lib.tdvd_vm_session_notify_playback.restype = None

        lib.spu_rgba_to_png.argtypes = [
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.spu_rgba_to_png.restype = ctypes.c_void_p

        lib.spu_rgba_to_data_url.argtypes = [
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_int,
            ctypes.c_int,
        ]
        lib.spu_rgba_to_data_url.restype = ctypes.c_void_p

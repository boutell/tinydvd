#!/usr/bin/env python3
"""
ctypes bindings to the C media & canvas primitive engine (libtinydvd_core).

Keeps clean-room IFO/NV_PCK VM execution, SPU RLE compositing, libmpv rendering,
and low-level SDL2/OpenGL/Cairo/Pango drawing & event primitives in C while
running all application logic, desktop UI layout/widgets, library management,
.tdvd archive parsing, OS directory watching, and HTTP/SSE/WS serving in Python.
"""

from __future__ import annotations

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
        os.path.join(repo_root, f"libtinydvd_core{ext}"),
        os.path.join(os.getcwd(), "build", f"libtinydvd_core{ext}"),
        os.path.join(os.path.dirname(sys.executable), f"libtinydvd_core{ext}"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    raise FileNotFoundError(f"Could not locate libtinydvd_core{ext} in build/ or TINYDVD_CORE_LIB")


TDVD_VM_JSON_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p)


class DesktopUIEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("button", ctypes.c_int),
        ("clicks", ctypes.c_int),
        ("motion_state", ctypes.c_uint32),
        ("wheel_y", ctypes.c_int),
        ("keysym", ctypes.c_int),
        ("scancode", ctypes.c_int),
        ("keymod", ctypes.c_uint16),
        ("text", ctypes.c_char * 64),
    ]


class TinyDVDCore:
    def __init__(self, lib_path: str | None = None):
        self.path = lib_path or find_core_library()
        self.lib = ctypes.CDLL(self.path)
        self._bind()

    def _bind(self) -> None:
        lib = self.lib

        lib.tdvd_free.argtypes = [ctypes.c_void_p]
        lib.tdvd_free.restype = None

        # Desktop Window, Player & Event Primitives
        lib.tdvd_desktop_create.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        lib.tdvd_desktop_create.restype = ctypes.c_void_p

        lib.tdvd_desktop_destroy.argtypes = [ctypes.c_void_p]
        lib.tdvd_desktop_destroy.restype = None

        lib.tdvd_desktop_quit_requested.argtypes = []
        lib.tdvd_desktop_quit_requested.restype = ctypes.c_int

        lib.tdvd_desktop_get_ticks.argtypes = []
        lib.tdvd_desktop_get_ticks.restype = ctypes.c_uint32

        lib.tdvd_desktop_get_sizes.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.tdvd_desktop_get_sizes.restype = None

        lib.tdvd_desktop_set_window_title.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.tdvd_desktop_set_window_title.restype = None

        lib.tdvd_desktop_set_fullscreen.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.tdvd_desktop_set_fullscreen.restype = None

        lib.tdvd_desktop_set_cursor_visible.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.tdvd_desktop_set_cursor_visible.restype = None

        lib.tdvd_desktop_get_clipboard.argtypes = []
        lib.tdvd_desktop_get_clipboard.restype = ctypes.c_void_p

        lib.tdvd_desktop_set_clipboard.argtypes = [ctypes.c_char_p]
        lib.tdvd_desktop_set_clipboard.restype = None

        lib.tdvd_desktop_choose_source_dialog.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.tdvd_desktop_choose_source_dialog.restype = ctypes.c_int

        lib.tdvd_desktop_poll_event.argtypes = [ctypes.c_void_p, ctypes.POINTER(DesktopUIEvent)]
        lib.tdvd_desktop_poll_event.restype = ctypes.c_int

        lib.tdvd_desktop_start_movie.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        lib.tdvd_desktop_start_movie.restype = ctypes.c_int

        lib.tdvd_desktop_stop_movie.argtypes = [ctypes.c_void_p]
        lib.tdvd_desktop_stop_movie.restype = None

        lib.tdvd_desktop_is_vm_running.argtypes = [ctypes.c_void_p]
        lib.tdvd_desktop_is_vm_running.restype = ctypes.c_int

        lib.tdvd_desktop_vm_action.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_double,
        ]
        lib.tdvd_desktop_vm_action.restype = ctypes.c_int

        lib.tdvd_desktop_present_frame.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        lib.tdvd_desktop_present_frame.restype = None

        # Cairo / Pango / OpenGL 2D Canvas Primitives
        lib.tdvd_canvas_begin.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
        ]
        lib.tdvd_canvas_begin.restype = ctypes.c_int

        lib.tdvd_canvas_end.argtypes = [ctypes.c_void_p]
        lib.tdvd_canvas_end.restype = None

        lib.tdvd_canvas_save.argtypes = [ctypes.c_void_p]
        lib.tdvd_canvas_save.restype = None

        lib.tdvd_canvas_restore.argtypes = [ctypes.c_void_p]
        lib.tdvd_canvas_restore.restype = None

        lib.tdvd_canvas_clip_rect.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_int,
        ]
        lib.tdvd_canvas_clip_rect.restype = None

        lib.tdvd_canvas_radial_bg.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
        ]
        lib.tdvd_canvas_radial_bg.restype = None

        lib.tdvd_canvas_linear_gradient_rect.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
        ]
        lib.tdvd_canvas_linear_gradient_rect.restype = None

        lib.tdvd_canvas_rounded_rect.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_int,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
        ]
        lib.tdvd_canvas_rounded_rect.restype = None

        lib.tdvd_canvas_text.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_int,
            ctypes.c_double,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_int,
            ctypes.c_int,
        ]
        lib.tdvd_canvas_text.restype = ctypes.c_double

        lib.tdvd_canvas_measure_text.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_double,
            ctypes.c_int,
            ctypes.c_int,
        ]
        lib.tdvd_canvas_measure_text.restype = ctypes.c_double

        lib.tdvd_canvas_text_x_to_byte_index.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_double,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_double,
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.tdvd_canvas_text_x_to_byte_index.restype = ctypes.c_int

        lib.tdvd_canvas_draw_log_block.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
        ]
        lib.tdvd_canvas_draw_log_block.restype = None

        lib.tdvd_canvas_search_icon.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
        ]
        lib.tdvd_canvas_search_icon.restype = None

        lib.tdvd_canvas_x_icon.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
        ]
        lib.tdvd_canvas_x_icon.restype = None

        lib.tdvd_canvas_cache_png.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_double,
        ]
        lib.tdvd_canvas_cache_png.restype = ctypes.c_int

        lib.tdvd_canvas_draw_cached_png.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
        ]
        lib.tdvd_canvas_draw_cached_png.restype = ctypes.c_int

        lib.tdvd_canvas_clear_png_cache.argtypes = [ctypes.c_void_p]
        lib.tdvd_canvas_clear_png_cache.restype = None

        # Headless DVD VM Session Bridge (Web)
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

        # SPU RGBA -> PNG
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

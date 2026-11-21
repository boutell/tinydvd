#!/usr/bin/env python3
"""
Pure-Python desktop UI layer for TinyDVD Cinema.

Owns all desktop UI state, UTF-8 text editing/selection/clipboard, search
filtering, card grid layout & scrolling, mouse hit-testing, player HUD, and
Import DVD modal logic, driving low-level Cairo/Pango/OpenGL/SDL2/mpv/DVD-VM
primitives exposed by libtinydvd_core.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
import os
import shutil
import struct
import subprocess
import sys
import threading
import time

from tinydvd_core import DesktopUIEvent, TinyDVDCore
import tinydvd_archive
from tinydvd_library import (
    IMPORT_DONE,
    IMPORT_ERROR,
    IMPORT_IDLE,
    CatalogItem,
    TinyDVDLibrary,
    detect_default_source,
    is_dvd_dir,
)

TINYDVD_VERSION = "1.0.0"
CURSOR_IDLE_MS = 2000

FOCUS_NONE = 0
FOCUS_SEARCH = 1
FOCUS_MODAL_SOURCE = 2
FOCUS_MODAL_TITLE = 3

ALIGN_LEFT = 0
ALIGN_CENTER = 1
ALIGN_RIGHT = 2

# SDL2 constants (cross-platform values from SDL_keycode.h / SDL_scancode.h / SDL_mouse.h)
SDLK_SCANCODE_MASK = 1 << 30
SDLK_RETURN = ord("\r")
SDLK_ESCAPE = 0x1B
SDLK_BACKSPACE = ord("\b")
SDLK_TAB = ord("\t")
SDLK_SPACE = ord(" ")
SDLK_COMMA = ord(",")
SDLK_PERIOD = ord(".")
SDLK_LEFTBRACKET = ord("[")
SDLK_RIGHTBRACKET = ord("]")
SDLK_DELETE = 0x7F
SDLK_0 = ord("0")
SDLK_9 = ord("9")
SDLK_a = ord("a")
SDLK_c = ord("c")
SDLK_f = ord("f")
SDLK_m = ord("m")
SDLK_q = ord("q")
SDLK_s = ord("s")
SDLK_u = ord("u")
SDLK_v = ord("v")
SDLK_x = ord("x")

SDLK_HOME = 74 | SDLK_SCANCODE_MASK
SDLK_PAGEUP = 75 | SDLK_SCANCODE_MASK
SDLK_END = 77 | SDLK_SCANCODE_MASK
SDLK_PAGEDOWN = 78 | SDLK_SCANCODE_MASK
SDLK_RIGHT = 79 | SDLK_SCANCODE_MASK
SDLK_LEFT = 80 | SDLK_SCANCODE_MASK
SDLK_DOWN = 81 | SDLK_SCANCODE_MASK
SDLK_UP = 82 | SDLK_SCANCODE_MASK
SDLK_KP_ENTER = 88 | SDLK_SCANCODE_MASK

# Player keys: (keys, shift_only, VM action, argument). The first matching
# row wins, so the Shift rows come first. 0-9 seek to 0%-90% (see
# player_key_action). Q, F and Escape are handled by the app itself.
PLAYER_KEYS = [
    ((SDLK_LEFT,), True, b"seek_rel", -60.0),
    ((SDLK_RIGHT,), True, b"seek_rel", 60.0),
    ((SDLK_UP,), False, b"up", 0.0),
    ((SDLK_DOWN,), False, b"down", 0.0),
    ((SDLK_LEFT,), False, b"left", 0.0),
    ((SDLK_RIGHT,), False, b"right", 0.0),
    ((SDLK_COMMA,), False, b"seek_rel", -10.0),
    ((SDLK_PERIOD,), False, b"seek_rel", 10.0),
    ((SDLK_LEFTBRACKET, SDLK_PAGEUP), False, b"prev_chapter", 0.0),
    ((SDLK_RIGHTBRACKET, SDLK_PAGEDOWN), False, b"next_chapter", 0.0),
    ((SDLK_c, SDLK_s), False, b"cycle_subs", 0.0),
    ((SDLK_a,), False, b"cycle_audio", 0.0),
    ((SDLK_RETURN, SDLK_KP_ENTER), False, b"activate", 0.0),
    ((SDLK_SPACE,), False, b"pause", 0.0),
    ((SDLK_m,), False, b"menu", 0.0),
]

# Help for the keys above, shown by `tinydvd --help`. Keep the two in step.
PLAYER_KEY_HELP = [
    ("Arrow keys", "In a menu: move between buttons"),
    ("Left / Right", "In a title: seek -10s / +10s"),
    ("Shift + Left / Right", "Seek -60s / +60s"),
    ("Down / Up", "In a title: seek -60s / +60s"),
    (", / .", "Seek -10s / +10s"),
    ("[ / ] or PgUp / PgDn", "Previous / next chapter"),
    ("0 - 9", "Jump to 0% - 90% of the title"),
    ("Enter / mouse click", "Activate the selected menu button"),
    ("Space", "Toggle pause"),
    ("C / S", "Cycle subtitles and closed captions"),
    ("A", "Cycle audio tracks"),
    ("M", "Go to the disc's root menu"),
    ("Escape", "Go up a menu level, or back to the library"),
    ("F", "Toggle fullscreen"),
    ("Q", "Quit"),
]


def player_key_action(sym: int, mod: int) -> tuple[bytes, float] | None:
    """(VM action, argument) for a key pressed while a disc plays, or None."""
    if SDLK_0 <= sym <= SDLK_9:
        return b"seek_frac", float(sym - SDLK_0) * 0.10
    shifted = bool(mod & KMOD_SHIFT)
    for keys, shift_only, action, arg in PLAYER_KEYS:
        if sym in keys and (shifted or not shift_only):
            return action, arg
    return None

SDL_SCANCODE_A = 4
SDL_SCANCODE_C = 6
SDL_SCANCODE_F = 9
SDL_SCANCODE_U = 24
SDL_SCANCODE_V = 25
SDL_SCANCODE_X = 27

KMOD_SHIFT = 0x0003
KMOD_CTRL = 0x00C0
KMOD_ALT = 0x0300
KMOD_GUI = 0x0C00

SDL_BUTTON_LEFT = 1
SDL_BUTTON_LMASK = 1

EVT_NONE = 0
EVT_QUIT = 1
EVT_MOUSE_MOTION = 2
EVT_MOUSE_DOWN = 3
EVT_MOUSE_UP = 4
EVT_MOUSE_WHEEL = 5
EVT_KEYDOWN = 6
EVT_TEXTINPUT = 7
EVT_FILE_OPENED = 9  # dropped on the window, or opened from Finder

_PNG_SIG = b"\x89PNG\r\n\x1a\n"


def parse_png_sar(data: bytes) -> float:
    """Parse sample aspect ratio from PNG pHYs chunk if present (0.5 <= sar <= 2.5)."""
    if not data or len(data) < 20 or not data.startswith(_PNG_SIG):
        return 1.0
    pos = 8
    size = len(data)
    while pos + 12 <= size:
        clen = struct.unpack_from(">I", data, pos)[0]
        ctype = data[pos + 4 : pos + 8]
        if ctype == b"pHYs" and clen == 9 and pos + 8 + 9 <= size:
            px, py, _unit = struct.unpack_from(">IIB", data, pos + 8)
            if px > 0 and py > 0:
                sar = float(px) / float(py)
                if 0.5 <= sar <= 2.5:
                    return sar
        if ctype in (b"IDAT", b"IEND"):
            break
        if clen > size - pos - 12:
            break
        pos += 12 + clen
    return 1.0


def choose_source_dialog(core: TinyDVDCore, choose_iso_only: bool, initial_path: str = "") -> str:
    """Open native OS file/folder dialog to select a DVD source."""
    if sys.platform == "darwin":
        buf = ctypes.create_string_buffer(1024)
        ok = core.lib.tdvd_desktop_choose_source_dialog(
            1 if choose_iso_only else 0,
            (initial_path or "").encode("utf-8"),
            buf,
            ctypes.sizeof(buf),
        )
        return buf.value.decode("utf-8", errors="replace") if ok else ""

    start_dir = initial_path if (initial_path and not initial_path.startswith("/dev/")) else "."
    cmds = (
        [
            [
                "zenity",
                "--file-selection",
                "--title=Select DVD ISO Image",
                "--file-filter=DVD Disc Images | *.iso *.ISO *.img *.IMG",
            ],
            ["kdialog", "--getopenfilename", start_dir, "*.iso *.img|DVD Images"],
        ]
        if choose_iso_only
        else [
            [
                "zenity",
                "--file-selection",
                "--directory",
                "--title=Select DVD Folder (VIDEO_TS or Rip)",
            ],
            ["kdialog", "--getexistingdirectory", start_dir],
        ]
    )
    for argv in cmds:
        if not shutil.which(argv[0]):
            continue
        try:
            res = subprocess.run(argv, capture_output=True, text=True, check=False)
            out = (res.stdout or "").rstrip("\r\n")
            if res.returncode == 0 and out:
                return out
        except OSError:
            continue
    return ""


@dataclass(frozen=True)
class Rect:
    x: float = 0.0
    y: float = 0.0
    w: float = 0.0
    h: float = 0.0

    def contains(self, px: float, py: float) -> bool:
        return (
            self.w > 0.0
            and self.h > 0.0
            and self.x <= px <= (self.x + self.w)
            and self.y <= py <= (self.y + self.h)
        )


def _is_word_char(ch: str) -> bool:
    return ch.isalnum() or ch == "_"


class TextField:
    """Single-line UTF-8 text input with selection, word/line navigation, and clipboard."""

    MAX_CHARS = 511

    def __init__(self, initial: str = ""):
        self.text: str = ""
        self.cursor: int = 0
        self.sel_anchor: int = -1
        self.scroll_x: float = 0.0
        if initial:
            self.set_text(initial)

    def has_selection(self) -> bool:
        return 0 <= self.sel_anchor != self.cursor

    def get_selection(self) -> tuple[int, int]:
        if self.sel_anchor < 0:
            return self.cursor, self.cursor
        a = max(0, min(self.sel_anchor, len(self.text)))
        b = max(0, min(self.cursor, len(self.text)))
        return (a, b) if a <= b else (b, a)

    def clear_selection(self) -> None:
        self.sel_anchor = -1

    def select_all(self) -> None:
        self.sel_anchor = 0
        self.cursor = len(self.text)

    def delete_selection(self) -> None:
        if not self.has_selection():
            self.sel_anchor = -1
            return
        s, e = self.get_selection()
        if e > s:
            self.text = self.text[:s] + self.text[e:]
        self.cursor = s
        self.sel_anchor = -1

    def prev_word_pos(self, pos: int) -> int:
        p = max(0, min(pos, len(self.text)))
        while p > 0 and not _is_word_char(self.text[p - 1]):
            p -= 1
        while p > 0 and _is_word_char(self.text[p - 1]):
            p -= 1
        return p

    def next_word_pos(self, pos: int) -> int:
        n = len(self.text)
        p = max(0, min(pos, n))
        while p < n and not _is_word_char(self.text[p]):
            p += 1
        while p < n and _is_word_char(self.text[p]):
            p += 1
        return p

    def move_cursor(self, new_pos: int, extend_selection: bool) -> None:
        clamped = max(0, min(new_pos, len(self.text)))
        if extend_selection:
            if self.sel_anchor < 0:
                self.sel_anchor = self.cursor
            self.cursor = clamped
            if self.cursor == self.sel_anchor:
                self.sel_anchor = -1
        else:
            self.sel_anchor = -1
            self.cursor = clamped

    def set_text(self, s: str) -> None:
        clean = (s or "").replace("\r", "").replace("\n", "")[: self.MAX_CHARS]
        self.text = clean
        self.cursor = len(self.text)
        self.sel_anchor = -1
        self.scroll_x = 0.0

    def insert_str(self, ins: str) -> None:
        if not ins:
            return
        if self.has_selection():
            self.delete_selection()
        else:
            self.sel_anchor = -1

        clean = ins.replace("\r", "").replace("\n", "")
        if not clean:
            return
        avail = self.MAX_CHARS - len(self.text)
        if avail <= 0:
            return
        clean = clean[:avail]
        self.text = self.text[: self.cursor] + clean + self.text[self.cursor :]
        self.cursor += len(clean)

    def backspace(self, word_mod: bool = False, line_mod: bool = False) -> None:
        if not self.text:
            return
        if self.has_selection():
            self.delete_selection()
            return
        self.sel_anchor = -1
        if self.cursor <= 0:
            return
        if line_mod:
            prev = 0
        elif word_mod:
            prev = self.prev_word_pos(self.cursor)
        else:
            prev = self.cursor - 1
        if prev < self.cursor:
            self.text = self.text[:prev] + self.text[self.cursor :]
            self.cursor = prev

    def delete_fwd(self, word_mod: bool = False) -> None:
        if not self.text:
            return
        if self.has_selection():
            self.delete_selection()
            return
        self.sel_anchor = -1
        if self.cursor >= len(self.text):
            return
        nxt = self.next_word_pos(self.cursor) if word_mod else (self.cursor + 1)
        if nxt > self.cursor:
            self.text = self.text[: self.cursor] + self.text[nxt:]

    def copy_selection(self, core: TinyDVDCore) -> None:
        if self.has_selection():
            s, e = self.get_selection()
            core.lib.tdvd_desktop_set_clipboard(self.text[s:e].encode("utf-8"))
        elif self.text:
            core.lib.tdvd_desktop_set_clipboard(self.text.encode("utf-8"))

    def cut_selection(self, core: TinyDVDCore) -> bool:
        if not self.has_selection():
            return False
        self.copy_selection(core)
        self.delete_selection()
        return True

    def x_to_char_index(
        self,
        core: TinyDVDCore,
        ctx,
        rel_x: float,
        font_px: float = 13.5,
        bold: bool = False,
        monospace: bool = False,
    ) -> int:
        if not self.text:
            return 0
        target_x = rel_x + self.scroll_x
        if target_x <= 0.0:
            return 0
        text_bytes = self.text.encode("utf-8")
        trailing = ctypes.c_int(0)
        bidx = core.lib.tdvd_canvas_text_x_to_byte_index(
            ctx,
            text_bytes,
            float(font_px),
            1 if bold else 0,
            1 if monospace else 0,
            float(target_x),
            ctypes.byref(trailing),
        )
        prefix_chars = len(text_bytes[: max(0, bidx)].decode("utf-8", errors="ignore"))
        return max(0, min(len(self.text), prefix_chars + trailing.value))


class TinyDVDDesktopUI:
    """Manages desktop UI layout, rendering, filtering, modals, and input events."""

    def __init__(self, core: TinyDVDCore, ctx, lib: TinyDVDLibrary):
        self.core = core
        self.ctx = ctx
        self.lib = lib

        self.dirty = True
        self.last_win_w = 0
        self.last_win_h = 0
        self.last_draw_w = 0
        self.last_draw_h = 0
        self.last_player_view = False
        self.last_hud_visible = False
        self.last_tick_ms = 0
        self.last_catalog_gen = -1

        self.focus = FOCUS_NONE
        self.tf_search = TextField()
        self.tf_source = TextField()
        self.tf_title = TextField()
        self.enrich_checked = True
        self.dragging_tf = False

        self.items_snapshot: list[CatalogItem] = []
        self.filtered_indices: list[int] = []
        self.selected_filtered = -1
        self.scroll_y = 0.0
        self.max_scroll_y = 0.0
        self.hovered_card = -1
        self.mouse_x = 0
        self.mouse_y = 0

        self.import_modal_open = False
        self.import_prefilled = False
        self.last_import_state = IMPORT_IDLE
        self.last_import_active = False
        self.local_import_err = ""
        self.log_scroll_offset = 0.0
        self.copy_log_feedback_ms = 0

        self.attempted_thumbs: set[str] = set()
        self.card_hits: list[tuple[Rect, int]] = []

        # Layout rects in logical window coordinates
        self.r_search = Rect()
        self.r_search_clear = Rect()
        self.r_open_import = Rect()
        self.r_empty_import = Rect()
        self.r_back_lib = Rect()

        self.r_modal_box = Rect()
        self.r_modal_close = Rect()
        self.r_modal_source = Rect()
        self.r_modal_browse = Rect()
        self.r_modal_browse_iso = Rect()
        self.r_modal_title = Rect()
        self.r_modal_enrich = Rect()
        self.r_modal_copy_log = Rect()
        self.r_modal_start = Rect()
        self.r_modal_cancel = Rect()
        self.r_modal_done = Rect()
        self.r_modal_play = Rect()

        self.sync_catalog_snapshot()
        self.last_catalog_gen = self.lib.get_generation()

    def mark_dirty(self) -> None:
        self.dirty = True

    def _read_import_snapshot(self) -> dict:
        with self.lib.import_lock:
            elapsed = int(time.time() - self.lib.imp_start_time) if self.lib.imp_start_time > 0 else 0
            return {
                "state": self.lib.imp_state,
                "active": self.lib.imp_active,
                "elapsed_sec": elapsed,
                "stage_msg": self.lib.imp_stage_msg,
                "error_msg": self.lib.imp_error_msg,
                "result_disc_id": self.lib.imp_result_disc_id,
                "result_disc_path": self.lib.imp_result_disc_path,
                "result_title": self.lib.imp_result_title,
                "logs": list(self.lib.imp_logs),
            }

    def sync_catalog_snapshot(self) -> None:
        _is_lib, items, _tpath = self.lib.get_snapshot()
        if items != self.items_snapshot:
            self.items_snapshot = items
            self.dirty = True

        q = self.tf_search.text.strip().lower()
        filtered: list[int] = []
        for idx, it in enumerate(self.items_snapshot):
            if not q:
                filtered.append(idx)
                continue
            year_str = str(it.year) if it.year > 0 else ""
            if (
                q in it.title.lower()
                or q in it.director.lower()
                or q in it.cast.lower()
                or q in it.genres.lower()
                or q in it.overview.lower()
                or (year_str and q in year_str)
            ):
                filtered.append(idx)

        self.filtered_indices = filtered
        if self.selected_filtered >= len(self.filtered_indices):
            self.selected_filtered = len(self.filtered_indices) - 1

    def open_import_modal(self) -> None:
        self.import_modal_open = True
        self.dragging_tf = False
        self.copy_log_feedback_ms = 0
        self.local_import_err = ""

        st = self._read_import_snapshot()
        active = st["active"]
        if not active and not self.import_prefilled:
            def_src, def_title = detect_default_source()
            if not self.tf_source.text and def_src:
                self.tf_source.set_text(def_src)
            if not self.tf_title.text and def_title:
                self.tf_title.set_text(def_title)
            self.import_prefilled = True

        self.focus = FOCUS_NONE if active else FOCUS_MODAL_SOURCE
        self.dirty = True

    def close_import_modal(self) -> None:
        self.import_modal_open = False
        self.dragging_tf = False
        if self.focus in (FOCUS_MODAL_SOURCE, FOCUS_MODAL_TITLE):
            self.focus = FOCUS_NONE
        self.dirty = True

    def start_import_from_modal(self) -> None:
        src = self.tf_source.text.strip()
        if not src:
            self.local_import_err = "Please enter a DVD drive device or source path."
            self.dirty = True
            return
        self.local_import_err = ""
        ok, err = self.lib.start_import(
            src,
            title=self.tf_title.text,
            skip_enrichment=not self.enrich_checked,
        )
        if not ok:
            self.local_import_err = err or "Failed to start import."
        else:
            self.focus = FOCUS_NONE
            self.dragging_tf = False
        self.dirty = True

    def copy_import_log_to_clipboard(self) -> None:
        st = self._read_import_snapshot()
        log_concat = "\n".join(st["logs"])
        disp_err = self.local_import_err or st["error_msg"]
        stage_msg = st["stage_msg"]
        if log_concat and disp_err:
            combined = f"{log_concat}\nError: {disp_err}"
        elif log_concat:
            combined = log_concat
        elif disp_err:
            combined = disp_err
        elif stage_msg:
            combined = stage_msg
        else:
            return
        self.core.lib.tdvd_desktop_set_clipboard(combined.encode("utf-8"))
        now = int(self.core.lib.tdvd_desktop_get_ticks())
        self.copy_log_feedback_ms = 1 if now == 0 else now
        self.dirty = True

    def _active_textfield(self) -> TextField | None:
        if self.focus == FOCUS_SEARCH:
            return self.tf_search
        if self.focus == FOCUS_MODAL_SOURCE:
            return self.tf_source
        if self.focus == FOCUS_MODAL_TITLE:
            return self.tf_title
        return None

    def _textfield_geometry(self, focus: int) -> tuple[Rect, float] | None:
        if focus == FOCUS_SEARCH:
            return self.r_search, 32.0
        if focus == FOCUS_MODAL_SOURCE:
            return self.r_modal_source, 12.0
        if focus == FOCUS_MODAL_TITLE:
            return self.r_modal_title, 12.0
        return None

    def _handle_textfield_mouse_down(self, target_focus: int, x: int, clicks: int, keymod: int = 0) -> None:
        self.focus = target_focus
        tf = self._active_textfield()
        geom = self._textfield_geometry(target_focus)
        if not tf or not geom:
            self.dragging_tf = False
            self.dirty = True
            return
        r, left_pad = geom
        idx = tf.x_to_char_index(self.core, self.ctx, float(x) - (r.x + left_pad))
        if clicks >= 3:
            tf.select_all()
            self.dragging_tf = False
        elif clicks == 2:
            start_anchor = min(len(tf.text), idx + 1)
            tf.sel_anchor = tf.prev_word_pos(start_anchor)
            tf.cursor = tf.next_word_pos(idx)
            if tf.cursor == tf.sel_anchor:
                tf.sel_anchor = -1
            self.dragging_tf = False
        else:
            if keymod & KMOD_SHIFT:
                tf.move_cursor(idx, True)
                if tf.sel_anchor < 0:
                    tf.sel_anchor = tf.cursor
            else:
                tf.sel_anchor = idx
                tf.cursor = idx
            self.dragging_tf = True
        self.dirty = True

    def compute_layout_rects(self, win_w: int, win_h: int, in_player_view: bool) -> None:
        self.r_search = Rect()
        self.r_search_clear = Rect()
        self.r_open_import = Rect()
        self.r_empty_import = Rect()
        self.r_back_lib = Rect()
        self.r_modal_box = Rect()
        self.r_modal_close = Rect()
        self.r_modal_source = Rect()
        self.r_modal_browse = Rect()
        self.r_modal_browse_iso = Rect()
        self.r_modal_title = Rect()
        self.r_modal_enrich = Rect()
        self.r_modal_copy_log = Rect()
        self.r_modal_start = Rect()
        self.r_modal_cancel = Rect()
        self.r_modal_done = Rect()
        self.r_modal_play = Rect()
        self.card_hits = []

        if in_player_view:
            self.r_back_lib = Rect(16.0, 12.0, 108.0, 34.0)
        else:
            btn_w = 138.0
            self.r_open_import = Rect(float(win_w) - 24.0 - btn_w, 18.0, btn_w, 36.0)

            sw = min(380.0, max(160.0, float(win_w) - 520.0))
            left_bound = 236.0
            right_bound = self.r_open_import.x - 110.0
            sx = left_bound + ((right_bound - left_bound) - sw) * 0.5
            if sx < left_bound:
                sx = left_bound
            if sx + sw > right_bound:
                sw = max(120.0, right_bound - sx)
            self.r_search = Rect(sx, 18.0, sw, 36.0)
            if self.tf_search.text:
                self.r_search_clear = Rect(sx + sw - 32.0, 18.0, 28.0, 36.0)

            if not self.items_snapshot:
                self.r_empty_import = Rect((float(win_w) - 160.0) * 0.5, 220.0, 160.0, 38.0)
            elif self.filtered_indices:
                pad = 24.0
                gap = 20.0
                grid_top = 88.0
                avail_w = max(220.0, float(win_w) - pad * 2.0)
                cols = max(1, int((avail_w + gap) / 225.0))
                card_w = (avail_w - (cols - 1) * gap) / cols
                thumb_h = min(card_w * (4.0 / 3.0), 360.0)
                info_h = 148.0
                card_h = thumb_h + info_h

                n = len(self.filtered_indices)
                rows = (n + cols - 1) // cols
                total_h = rows * card_h + (rows - 1) * gap + pad
                view_h = max(100.0, float(win_h) - grid_top)
                self.max_scroll_y = max(0.0, total_h - view_h)
                self.scroll_y = max(0.0, min(self.scroll_y, self.max_scroll_y))

                for fi, cat_idx in enumerate(self.filtered_indices):
                    r_idx = fi // cols
                    c_idx = fi % cols
                    cx = pad + c_idx * (card_w + gap)
                    cy = grid_top + r_idx * (card_h + gap) - self.scroll_y
                    self.card_hits.append((Rect(cx, cy, card_w, card_h), cat_idx))

        if self.import_modal_open:
            imp = self._read_import_snapshot()
            st = imp["state"]
            active = imp["active"]
            show_form = not active
            show_progress = (st != IMPORT_IDLE) or active or bool(self.local_import_err)

            mw = min(580.0, float(win_w) - 40.0)
            mh = 112.0 + (202.0 if show_form else 0.0) + (236.0 if show_progress else 0.0)
            if mh > float(win_h) - 32.0:
                mh = float(win_h) - 32.0

            mx = (float(win_w) - mw) * 0.5
            my = (float(win_h) - mh) * 0.5
            self.r_modal_box = Rect(mx, my, mw, mh)
            self.r_modal_close = Rect(mx + mw - 48.0, my + 18.0, 28.0, 28.0)

            cur_y = my + 64.0
            inner_x = mx + 24.0
            inner_w = mw - 48.0

            if show_form:
                cur_y += 22.0
                if sys.platform == "darwin":
                    browse_w = 96.0
                    self.r_modal_source = Rect(inner_x, cur_y, inner_w - browse_w - 8.0, 36.0)
                    self.r_modal_browse = Rect(inner_x + inner_w - browse_w, cur_y, browse_w, 36.0)
                else:
                    browse_w = 88.0
                    iso_w = 72.0
                    self.r_modal_source = Rect(inner_x, cur_y, inner_w - browse_w - iso_w - 16.0, 36.0)
                    self.r_modal_browse = Rect(inner_x + inner_w - browse_w - iso_w - 8.0, cur_y, browse_w, 36.0)
                    self.r_modal_browse_iso = Rect(inner_x + inner_w - iso_w, cur_y, iso_w, 36.0)

                cur_y += 66.0 + 22.0
                self.r_modal_title = Rect(inner_x, cur_y, inner_w, 36.0)
                cur_y += 48.0
                self.r_modal_enrich = Rect(inner_x, cur_y, inner_w, 26.0)
                cur_y += 34.0

            if show_progress:
                pbox_h = my + mh - 66.0 - cur_y
                if pbox_h > 40.0:
                    self.r_modal_copy_log = Rect(inner_x + inner_w - 162.0, cur_y + 5.0, 82.0, 26.0)

            footer_y = my + mh - 52.0
            rx = mx + mw - 24.0
            if st == IMPORT_DONE and not active:
                pw = 122.0
                self.r_modal_play = Rect(rx - pw, footer_y, pw, 36.0)
                rx -= pw + 10.0
            if not active:
                sw_btn = 124.0
                self.r_modal_start = Rect(rx - sw_btn, footer_y, sw_btn, 36.0)
                rx -= sw_btn + 10.0
            cw = 84.0
            self.r_modal_done = Rect(rx - cw, footer_y, cw, 36.0)
            rx -= cw + 10.0
            if active:
                xw = 124.0
                self.r_modal_cancel = Rect(rx - xw, footer_y, xw, 36.0)

    def _draw_text(
        self,
        text: str,
        x: float,
        y: float,
        w: float,
        max_lines: int,
        font_px: float,
        bold: bool,
        monospace: bool,
        rgba: tuple[float, float, float, float],
        align: int,
        vcenter: bool = False,
    ) -> float:
        return float(
            self.core.lib.tdvd_canvas_text(
                self.ctx,
                (text or "").encode("utf-8"),
                float(x),
                float(y),
                float(w),
                int(max_lines),
                float(font_px),
                1 if bold else 0,
                1 if monospace else 0,
                float(rgba[0]),
                float(rgba[1]),
                float(rgba[2]),
                float(rgba[3]),
                int(align),
                1 if vcenter else 0,
            )
        )

    def _measure_text(self, text: str, font_px: float, bold: bool = False, monospace: bool = False) -> float:
        if not text:
            return 0.0
        return float(
            self.core.lib.tdvd_canvas_measure_text(
                self.ctx,
                text.encode("utf-8"),
                float(font_px),
                1 if bold else 0,
                1 if monospace else 0,
            )
        )

    def _draw_button(self, r: Rect, label: str, primary: bool, danger: bool, hovered: bool) -> None:
        if r.w <= 0.0 or r.h <= 0.0:
            return
        if primary:
            fill = (0.40, 0.99, 0.95, 1.0 if hovered else 0.90)
            stroke_w, stroke = 0.0, (0.0, 0.0, 0.0, 0.0)
            text_rgba = (0.043, 0.047, 0.063, 1.0)
        elif danger:
            fill = (1.0, 0.30, 0.30, 0.32 if hovered else 0.18)
            stroke_w, stroke = 1.0, (1.0, 0.30, 0.30, 0.55)
            text_rgba = (1.0, 0.50, 0.50, 1.0)
        else:
            fill = (0.12, 0.16, 0.20, 0.98 if hovered else 0.85)
            stroke_w = 1.0
            stroke = (0.40, 0.99, 0.95, 0.9) if hovered else (1.0, 1.0, 1.0, 0.18)
            text_rgba = (1.0, 1.0, 1.0, 1.0)

        self.core.lib.tdvd_canvas_rounded_rect(
            self.ctx,
            r.x,
            r.y,
            r.w,
            r.h,
            8.0,
            0,
            *fill,
            stroke_w,
            *stroke,
        )
        if label:
            self._draw_text(
                label,
                r.x + 6.0,
                r.y + r.h * 0.5,
                r.w - 12.0,
                1,
                13.5,
                primary or danger,
                False,
                text_rgba,
                ALIGN_CENTER,
                vcenter=True,
            )

    def _draw_textfield(
        self,
        r: Rect,
        tf: TextField,
        placeholder: str,
        focused: bool,
        rounded_pill: bool,
        left_pad: float,
        right_pad: float,
    ) -> None:
        radius = (r.h * 0.5) if rounded_pill else 8.0
        stroke = (0.40, 0.99, 0.95, 0.95) if focused else (1.0, 1.0, 1.0, 0.18)
        stroke_w = 1.6 if focused else 1.0
        self.core.lib.tdvd_canvas_rounded_rect(
            self.ctx,
            r.x,
            r.y,
            r.w,
            r.h,
            radius,
            0,
            0.043,
            0.047,
            0.063,
            1.0,
            stroke_w,
            *stroke,
        )

        inner_x = r.x + left_pad
        inner_w = r.w - left_pad - right_pad
        center_y = r.y + r.h * 0.5

        self.core.lib.tdvd_canvas_save(self.ctx)
        self.core.lib.tdvd_canvas_clip_rect(self.ctx, inner_x, r.y + 2.0, max(4.0, inner_w), r.h - 4.0, 4.0, 0)

        if not tf.text:
            tf.scroll_x = 0.0
            self._draw_text(
                placeholder,
                inner_x,
                center_y,
                inner_w,
                1,
                13.5,
                False,
                False,
                (0.53, 0.57, 0.69, 0.75),
                ALIGN_LEFT,
                vcenter=True,
            )
            if focused:
                self.core.lib.tdvd_canvas_rounded_rect(
                    self.ctx, inner_x + 1.0, center_y - 8.0, 1.5, 16.0, 0.0, 0,
                    0.40, 0.99, 0.95, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0
                )
        else:
            cur_px = self._measure_text(tf.text[: tf.cursor], 13.5)
            full_w = self._measure_text(tf.text, 13.5)
            vis_w = max(12.0, inner_w - 6.0)

            if cur_px < tf.scroll_x:
                tf.scroll_x = cur_px
            if cur_px > tf.scroll_x + vis_w:
                tf.scroll_x = cur_px - vis_w
            if full_w <= vis_w:
                tf.scroll_x = 0.0
            elif tf.scroll_x > full_w - vis_w:
                tf.scroll_x = full_w - vis_w
            if tf.scroll_x < 0.0:
                tf.scroll_x = 0.0

            if focused and tf.has_selection():
                sel_s, sel_e = tf.get_selection()
                start_px = self._measure_text(tf.text[:sel_s], 13.5)
                end_px = self._measure_text(tf.text[:sel_e], 13.5)
                hx = inner_x + start_px - tf.scroll_x
                hw = max(2.0, end_px - start_px)
                self.core.lib.tdvd_canvas_rounded_rect(
                    self.ctx, hx, center_y - 10.0, hw, 20.0, 0.0, 0,
                    0.40, 0.99, 0.95, 0.28, 0.0, 0.0, 0.0, 0.0, 0.0
                )

            self._draw_text(
                tf.text,
                inner_x - tf.scroll_x,
                center_y,
                0.0,
                1,
                13.5,
                False,
                False,
                (1.0, 1.0, 1.0, 0.98),
                ALIGN_LEFT,
                vcenter=True,
            )

            if focused:
                self.core.lib.tdvd_canvas_rounded_rect(
                    self.ctx, inner_x + cur_px - tf.scroll_x, center_y - 8.0, 1.5, 16.0, 0.0, 0,
                    0.40, 0.99, 0.95, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0
                )

        self.core.lib.tdvd_canvas_restore(self.ctx)

    def _draw_card_thumbnail(self, item: CatalogItem, x: float, y: float, w: float, h: float) -> bool:
        if not item.has_thumbnail:
            return False
        if item.id not in self.attempted_thumbs:
            self.attempted_thumbs.add(item.id)
            png_bytes = self.lib.read_thumbnail(item.path)
            if png_bytes:
                sar = parse_png_sar(png_bytes)
                self.core.lib.tdvd_canvas_cache_png(
                    self.ctx,
                    item.id.encode("utf-8"),
                    png_bytes,
                    len(png_bytes),
                    float(sar),
                )
        return bool(
            self.core.lib.tdvd_canvas_draw_cached_png(
                self.ctx, item.id.encode("utf-8"), float(x), float(y), float(w), float(h)
            )
        )

    def _render_library_view(self, win_w: int, win_h: int) -> None:
        self.core.lib.tdvd_canvas_radial_bg(self.ctx, win_w, win_h, 0.102, 0.118, 0.141, 0.043, 0.047, 0.063)

        # Clip movie grid below header (y >= 72)
        self.core.lib.tdvd_canvas_save(self.ctx)
        self.core.lib.tdvd_canvas_clip_rect(self.ctx, 0.0, 72.0, float(win_w), max(0.0, float(win_h) - 72.0), 0.0, 0)

        if not self.items_snapshot:
            self._draw_text(
                "No TinyDVD movies found in library.",
                24.0, 150.0, float(win_w) - 48.0, 2, 18.0, False, False,
                (0.77, 0.78, 0.78, 0.9), ALIGN_CENTER,
            )
            self._draw_text(
                "Place .tdvd files in this folder or click + Import DVD to rip & add a disc.",
                24.0, 182.0, float(win_w) - 48.0, 2, 13.5, False, False,
                (0.53, 0.57, 0.69, 0.9), ALIGN_CENTER,
            )
            self._draw_button(
                self.r_empty_import,
                "+ Import DVD",
                True,
                False,
                (not self.import_modal_open) and self.r_empty_import.contains(self.mouse_x, self.mouse_y),
            )
        elif not self.filtered_indices:
            self._draw_text(
                f'No movies found matching "{self.tf_search.text}"',
                24.0, 160.0, float(win_w) - 48.0, 2, 16.0, False, False,
                (0.77, 0.78, 0.78, 0.9), ALIGN_CENTER,
            )
        else:
            for i, (r, cat_idx) in enumerate(self.card_hits):
                if r.y + r.h < 72.0 or r.y > win_h:
                    continue
                item = self.items_snapshot[cat_idx]
                hovered = (not self.import_modal_open) and ((self.hovered_card == i) or (self.selected_filtered == i))

                stroke = (0.40, 0.99, 0.95, 0.95) if hovered else (1.0, 1.0, 1.0, 0.09)
                stroke_w = 1.8 if hovered else 1.0
                self.core.lib.tdvd_canvas_rounded_rect(
                    self.ctx, r.x, r.y, r.w, r.h, 10.0, 0,
                    0.122, 0.157, 0.200, 1.0, stroke_w, *stroke
                )

                info_h = 148.0
                thumb_h = r.h - info_h

                # Thumbnail area
                self.core.lib.tdvd_canvas_save(self.ctx)
                self.core.lib.tdvd_canvas_clip_rect(self.ctx, r.x + 1.0, r.y + 1.0, r.w - 2.0, thumb_h - 1.0, 9.0, 1)
                self.core.lib.tdvd_canvas_rounded_rect(
                    self.ctx, r.x + 1.0, r.y + 1.0, r.w - 2.0, thumb_h - 1.0, 0.0, 0,
                    0.078, 0.090, 0.102, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0
                )
                if not self._draw_card_thumbnail(item, r.x, r.y, r.w, thumb_h):
                    self._draw_text(
                        "DVD", r.x, r.y + thumb_h * 0.5 - 14.0, r.w, 1, 22.0, True, False,
                        (0.40, 0.99, 0.95, 0.35), ALIGN_CENTER,
                    )
                self.core.lib.tdvd_canvas_restore(self.ctx)

                if item.year > 0:
                    yr_r = Rect(r.x + r.w - 48.0, r.y + 8.0, 40.0, 20.0)
                    self.core.lib.tdvd_canvas_rounded_rect(
                        self.ctx, yr_r.x, yr_r.y, yr_r.w, yr_r.h, 5.0, 0,
                        0.0, 0.0, 0.0, 0.80, 1.0, 0.40, 0.99, 0.95, 0.45
                    )
                    self._draw_text(
                        str(item.year), yr_r.x, yr_r.y + yr_r.h * 0.5, yr_r.w, 1, 11.5, True, False,
                        (0.40, 0.99, 0.95, 1.0), ALIGN_CENTER, vcenter=True,
                    )

                # Info Block
                ix = r.x + 12.0
                iw = r.w - 24.0
                iy = r.y + thumb_h + 10.0

                self._draw_text(item.title, ix, iy, iw, 1, 15.0, True, False, (1.0, 1.0, 1.0, 1.0), ALIGN_LEFT)
                iy += 22.0

                if item.director:
                    self._draw_text(
                        f"Dir. {item.director}", ix, iy, iw, 1, 12.0, False, False,
                        (0.27, 0.64, 0.62, 1.0), ALIGN_LEFT,
                    )
                    iy += 18.0

                if item.cast:
                    ch = self._draw_text(
                        item.cast, ix, iy, iw, 2, 11.5, False, False,
                        (0.53, 0.57, 0.69, 0.95), ALIGN_LEFT,
                    )
                    iy += min(ch, 32.0) + 6.0

                if item.genres and (iy + 22.0 < r.y + r.h - 26.0):
                    gx = ix
                    for raw_tok in item.genres.split(","):
                        tok = raw_tok.strip()
                        if not tok:
                            continue
                        tw = self._measure_text(tok, 10.5) + 12.0
                        if gx + tw > ix + iw:
                            break
                        self.core.lib.tdvd_canvas_rounded_rect(
                            self.ctx, gx, iy, tw, 17.0, 4.0, 0,
                            1.0, 1.0, 1.0, 0.08, 0.0, 0.0, 0.0, 0.0, 0.0
                        )
                        self._draw_text(
                            tok, gx + 6.0, iy + 2.0, tw - 12.0, 1, 10.5, False, False,
                            (0.77, 0.78, 0.78, 0.95), ALIGN_LEFT,
                        )
                        gx += tw + 5.0

                footer_cy = r.y + r.h - 17.0
                self._draw_text(
                    item.size_str, ix, footer_cy, iw * 0.6, 1, 12.0, False, False,
                    (0.77, 0.78, 0.78, 0.85), ALIGN_LEFT, vcenter=True,
                )
                self._draw_text(
                    "Play ▶", ix, footer_cy, iw, 1, 12.5, True, False,
                    (0.40, 0.99, 0.95, 1.0), ALIGN_RIGHT, vcenter=True,
                )

            if self.max_scroll_y > 1.0:
                track_h = float(win_h) - 84.0
                ratio = track_h / (track_h + self.max_scroll_y)
                thumb_sz = max(32.0, track_h * ratio)
                thumb_y = 78.0 + (track_h - thumb_sz) * (self.scroll_y / self.max_scroll_y)
                self.core.lib.tdvd_canvas_rounded_rect(
                    self.ctx, float(win_w) - 9.0, thumb_y, 5.0, thumb_sz, 2.5, 0,
                    0.40, 0.99, 0.95, 0.35, 0.0, 0.0, 0.0, 0.0, 0.0
                )

        self.core.lib.tdvd_canvas_restore(self.ctx)

        # Header Bar
        self.core.lib.tdvd_canvas_rounded_rect(
            self.ctx, 0.0, 0.0, float(win_w), 72.0, 0.0, 0,
            0.067, 0.078, 0.102, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0
        )
        self.core.lib.tdvd_canvas_rounded_rect(
            self.ctx, 0.0, 71.0, float(win_w), 1.0, 0.0, 0,
            1.0, 1.0, 1.0, 0.10, 0.0, 0.0, 0.0, 0.0, 0.0
        )

        header_cy = 36.0
        self._draw_text("Tiny", 24.0, header_cy, 0.0, 1, 24.0, True, False, (1.0, 1.0, 1.0, 1.0), ALIGN_LEFT, vcenter=True)
        w_tiny = self._measure_text("Tiny", 24.0, True)
        self._draw_text("DVD", 24.0 + w_tiny, header_cy, 0.0, 1, 24.0, True, False, (0.40, 0.99, 0.95, 1.0), ALIGN_LEFT, vcenter=True)
        w_dvd = self._measure_text("DVD", 24.0, True)
        self._draw_text(" Cinema", 24.0 + w_tiny + w_dvd, header_cy, 0.0, 1, 24.0, True, False, (1.0, 1.0, 1.0, 1.0), ALIGN_LEFT, vcenter=True)

        self._draw_textfield(
            self.r_search,
            self.tf_search,
            "Search title, actor, director, genre...",
            self.focus == FOCUS_SEARCH,
            True,
            32.0,
            32.0 if self.tf_search.text else 14.0,
        )
        self.core.lib.tdvd_canvas_search_icon(self.ctx, self.r_search.x + 18.0, header_cy, 0.65, 0.70, 0.80, 0.80)
        if self.tf_search.text:
            x_hov = self.r_search_clear.contains(self.mouse_x, self.mouse_y)
            self.core.lib.tdvd_canvas_x_icon(
                self.ctx,
                self.r_search_clear.x + self.r_search_clear.w * 0.5,
                header_cy,
                4.2,
                1.6,
                1.0,
                1.0,
                1.0,
                1.0 if x_hov else 0.65,
            )

        q = self.tf_search.text.strip()
        n_total = len(self.items_snapshot)
        plural = "" if n_total == 1 else "s"
        stats = f"{len(self.filtered_indices)} of {n_total} Movie{plural}" if q else f"{n_total} Movie{plural}"
        self._draw_text(
            stats, self.r_open_import.x - 134.0, header_cy, 120.0, 1, 13.5, False, False,
            (0.77, 0.78, 0.78, 0.90), ALIGN_RIGHT, vcenter=True,
        )

        imp = self._read_import_snapshot()
        self._draw_button(
            self.r_open_import,
            "Importing..." if imp["active"] else "+ Import DVD",
            True,
            False,
            (not self.import_modal_open) and self.r_open_import.contains(self.mouse_x, self.mouse_y),
        )

    def _render_player_hud(self, win_w: int, current_movie_title: str) -> None:
        self.core.lib.tdvd_canvas_linear_gradient_rect(
            self.ctx, 0.0, 0.0, float(win_w), 68.0,
            0.0, 0.0, 0.0, 0.82,
            0.0, 0.0, 0.0, 0.0,
        )
        self._draw_button(
            self.r_back_lib,
            "← Library",
            False,
            False,
            (not self.import_modal_open) and self.r_back_lib.contains(self.mouse_x, self.mouse_y),
        )
        if current_movie_title:
            self._draw_text(
                current_movie_title,
                138.0,
                self.r_back_lib.y + self.r_back_lib.h * 0.5,
                float(win_w) - 160.0,
                1,
                15.0,
                True,
                False,
                (1.0, 1.0, 1.0, 0.95),
                ALIGN_LEFT,
                vcenter=True,
            )

    def _render_import_modal(self, win_w: int, win_h: int) -> None:
        self.core.lib.tdvd_canvas_rounded_rect(
            self.ctx, 0.0, 0.0, float(win_w), float(win_h), 0.0, 0,
            0.0, 0.0, 0.0, 0.72, 0.0, 0.0, 0.0, 0.0, 0.0
        )

        b = self.r_modal_box
        self.core.lib.tdvd_canvas_rounded_rect(
            self.ctx, b.x, b.y, b.w, b.h, 12.0, 0,
            0.122, 0.157, 0.200, 1.0, 1.2, 1.0, 1.0, 1.0, 0.22
        )

        modal_title_cy = self.r_modal_close.y + self.r_modal_close.h * 0.5
        self._draw_text(
            "Import DVD to Library", b.x + 24.0, modal_title_cy, b.w - 88.0, 1, 18.0, True, False,
            (1.0, 1.0, 1.0, 1.0), ALIGN_LEFT, vcenter=True,
        )
        self._draw_button(self.r_modal_close, "", False, False, self.r_modal_close.contains(self.mouse_x, self.mouse_y))
        self.core.lib.tdvd_canvas_x_icon(
            self.ctx,
            self.r_modal_close.x + self.r_modal_close.w * 0.5,
            modal_title_cy,
            4.5,
            1.7,
            1.0,
            1.0,
            1.0,
            0.95,
        )

        imp = self._read_import_snapshot()
        st = imp["state"]
        active = imp["active"]
        elapsed = imp["elapsed_sec"]
        stage_msg = imp["stage_msg"]
        err_msg = imp["error_msg"]
        log_concat = "\n".join(imp["logs"])

        ix = b.x + 24.0
        iw = b.w - 48.0
        cur_y = b.y + 64.0

        if not active:
            self._draw_text("DVD Drive or Source Path", ix, cur_y, iw, 1, 13.0, True, False, (1.0, 1.0, 1.0, 0.95), ALIGN_LEFT)
            self._draw_textfield(
                self.r_modal_source,
                self.tf_source,
                "/dev/sr0 or /path/to/DVD_FOLDER or movie.iso",
                self.focus == FOCUS_MODAL_SOURCE,
                False,
                12.0,
                10.0,
            )
            if sys.platform == "darwin":
                self._draw_button(
                    self.r_modal_browse, "Browse...", False, False,
                    self.r_modal_browse.contains(self.mouse_x, self.mouse_y),
                )
            else:
                self._draw_button(
                    self.r_modal_browse, "Folder...", False, False,
                    self.r_modal_browse.contains(self.mouse_x, self.mouse_y),
                )
                self._draw_button(
                    self.r_modal_browse_iso, "ISO...", False, False,
                    self.r_modal_browse_iso.contains(self.mouse_x, self.mouse_y),
                )

            self._draw_text(
                "Optical drive device, mounted disc volume, DVD backup folder, or .iso file.",
                ix, self.r_modal_source.y + 40.0, iw, 1, 11.5, False, False, (0.53, 0.57, 0.69, 0.95), ALIGN_LEFT,
            )
            self._draw_text(
                "Movie Title (optional)", ix, self.r_modal_title.y - 22.0, iw, 1, 13.0, True, False,
                (1.0, 1.0, 1.0, 0.95), ALIGN_LEFT,
            )
            self._draw_textfield(
                self.r_modal_title,
                self.tf_title,
                "Auto-detect from disc label (e.g. Eulogy)",
                self.focus == FOCUS_MODAL_TITLE,
                False,
                12.0,
                10.0,
            )

            cb = Rect(self.r_modal_enrich.x, self.r_modal_enrich.y + 4.0, 18.0, 18.0)
            if self.enrich_checked:
                self.core.lib.tdvd_canvas_rounded_rect(
                    self.ctx, cb.x, cb.y, cb.w, cb.h, 4.0, 0,
                    0.40, 0.99, 0.95, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0
                )
                self._draw_text("✓", cb.x, cb.y + 1.0, cb.w, 1, 13.0, True, False, (0.043, 0.047, 0.063, 1.0), ALIGN_CENTER)
            else:
                self.core.lib.tdvd_canvas_rounded_rect(
                    self.ctx, cb.x, cb.y, cb.w, cb.h, 4.0, 0,
                    0.043, 0.047, 0.063, 1.0, 1.2, 1.0, 1.0, 1.0, 0.35
                )
            self._draw_text(
                "Fetch movie metadata (director, cast, year, genres)",
                cb.x + 26.0, cb.y + cb.h * 0.5, iw - 28.0, 1, 13.0, False, False,
                (0.77, 0.78, 0.78, 0.98), ALIGN_LEFT, vcenter=True,
            )
            cur_y = self.r_modal_enrich.y + 34.0

        display_err = self.local_import_err or err_msg
        if st != IMPORT_IDLE or active or display_err:
            pbox_h = b.y + b.h - 66.0 - cur_y
            if pbox_h > 40.0:
                self.core.lib.tdvd_canvas_rounded_rect(
                    self.ctx, ix, cur_y, iw, pbox_h, 8.0, 0,
                    0.043, 0.047, 0.063, 1.0, 1.0, 1.0, 1.0, 1.0, 0.12
                )
                is_err = (st == IMPORT_ERROR) or bool(self.local_import_err)
                s_text = stage_msg or ("Error" if self.local_import_err else "Importing...")
                prog_cy = (
                    self.r_modal_copy_log.y + self.r_modal_copy_log.h * 0.5
                    if self.r_modal_copy_log.h > 0.0
                    else cur_y + 18.0
                )
                self._draw_text(
                    s_text, ix + 12.0, prog_cy, iw - 180.0, 1, 13.5, True, False,
                    (1.0, 0.30, 0.30, 1.0) if is_err else (0.40, 0.99, 0.95, 1.0),
                    ALIGN_LEFT, vcenter=True,
                )

                copy_lbl = "Copied!" if self.copy_log_feedback_ms != 0 else "Copy Log"
                self._draw_button(
                    self.r_modal_copy_log, copy_lbl, False, False,
                    self.r_modal_copy_log.contains(self.mouse_x, self.mouse_y),
                )

                tbuf = f"{elapsed // 60:02d}:{elapsed % 60:02d}"
                self._draw_text(
                    tbuf, ix + iw - 74.0, prog_cy, 62.0, 1, 13.0, False, True,
                    (0.77, 0.78, 0.78, 0.9), ALIGN_RIGHT, vcenter=True,
                )

                log_y = cur_y + 36.0
                if is_err and display_err:
                    self.core.lib.tdvd_canvas_rounded_rect(
                        self.ctx, ix + 12.0, log_y, iw - 24.0, 30.0, 5.0, 0,
                        1.0, 0.30, 0.30, 0.14, 0.0, 0.0, 0.0, 0.0, 0.0
                    )
                    self._draw_text(
                        display_err, ix + 20.0, log_y + 7.0, iw - 40.0, 1, 12.0, False, False,
                        (1.0, 0.45, 0.45, 1.0), ALIGN_LEFT,
                    )
                    log_y += 36.0

                log_h = cur_y + pbox_h - 10.0 - log_y
                if log_h > 24.0:
                    self.core.lib.tdvd_canvas_draw_log_block(
                        self.ctx,
                        log_concat.encode("utf-8"),
                        ix + 10.0,
                        log_y,
                        iw - 20.0,
                        log_h,
                        11.5,
                        float(self.log_scroll_offset),
                        0.66,
                        0.70,
                        0.82,
                        0.95,
                    )

        if active:
            self._draw_button(
                self.r_modal_cancel, "Cancel Import", False, True,
                self.r_modal_cancel.contains(self.mouse_x, self.mouse_y),
            )
        self._draw_button(
            self.r_modal_done, "Close", False, False,
            self.r_modal_done.contains(self.mouse_x, self.mouse_y),
        )
        if not active:
            self._draw_button(
                self.r_modal_start, "Start Import", True, False,
                self.r_modal_start.contains(self.mouse_x, self.mouse_y),
            )
        if st == IMPORT_DONE and not active:
            self._draw_button(
                self.r_modal_play, "Play Now ▶", True, False,
                self.r_modal_play.contains(self.mouse_x, self.mouse_y),
            )

    def render_if_needed(
        self,
        win_w: int,
        win_h: int,
        draw_w: int,
        draw_h: int,
        in_player_view: bool,
        current_movie_title: str,
        hud_visible: bool,
        import_changed_event: threading.Event,
    ) -> None:
        if win_w <= 0 or win_h <= 0 or draw_w <= 0 or draw_h <= 0:
            return

        imp = self._read_import_snapshot()
        imp_state = imp["state"]
        imp_active = imp["active"]
        self.last_import_active = imp_active

        if self.last_import_state != IMPORT_DONE and imp_state == IMPORT_DONE:
            self.sync_catalog_snapshot()
            self.dirty = True
        self.last_import_state = imp_state

        if import_changed_event.is_set():
            import_changed_event.clear()
            self.dirty = True

        now = int(self.core.lib.tdvd_desktop_get_ticks())
        cur_gen = self.lib.get_generation()
        if cur_gen != self.last_catalog_gen:
            self.last_catalog_gen = cur_gen
            self.attempted_thumbs.clear()
            self.core.lib.tdvd_canvas_clear_png_cache(self.ctx)
            self.sync_catalog_snapshot()
            self.dirty = True

        if imp_active and (now - self.last_tick_ms >= 250):
            self.last_tick_ms = now
            self.dirty = True

        if self.copy_log_feedback_ms != 0 and (now - self.copy_log_feedback_ms >= 1500):
            self.copy_log_feedback_ms = 0
            self.dirty = True

        if (
            win_w != self.last_win_w
            or win_h != self.last_win_h
            or in_player_view != self.last_player_view
            or hud_visible != self.last_hud_visible
            or draw_w != self.last_draw_w
            or draw_h != self.last_draw_h
        ):
            self.last_win_w = win_w
            self.last_win_h = win_h
            self.last_draw_w = draw_w
            self.last_draw_h = draw_h
            self.last_player_view = in_player_view
            self.last_hud_visible = hud_visible
            self.dirty = True

        if in_player_view and not hud_visible and not self.import_modal_open:
            return

        if not self.dirty:
            return

        if not in_player_view:
            self.sync_catalog_snapshot()
        self.compute_layout_rects(win_w, win_h, in_player_view)

        if self.core.lib.tdvd_canvas_begin(self.ctx, draw_w, draw_h, win_w, win_h):
            if not in_player_view:
                self._render_library_view(win_w, win_h)
            elif hud_visible:
                self._render_player_hud(win_w, current_movie_title)
            if self.import_modal_open:
                self._render_import_modal(win_w, win_h)
            self.core.lib.tdvd_canvas_end(self.ctx)
        self.dirty = False

    def handle_event(
        self, ev: DesktopUIEvent, win_w: int, win_h: int, in_player_view: bool
    ) -> tuple[bool, str, str, bool]:
        """
        Handle one SDL UI event.

        Returns (consumed, play_path, play_title, back_to_library).
        """
        if win_w > 0 and win_h > 0 and self.last_win_w == 0:
            self.compute_layout_rects(win_w, win_h, in_player_view)

        etype = ev.type

        if etype == EVT_MOUSE_MOTION:
            self.mouse_x = ev.x
            self.mouse_y = ev.y
            if self.dragging_tf:
                if ev.motion_state & SDL_BUTTON_LMASK:
                    tf = self._active_textfield()
                    geom = self._textfield_geometry(self.focus)
                    if tf and geom:
                        r, left_pad = geom
                        tf.cursor = tf.x_to_char_index(self.core, self.ctx, float(self.mouse_x) - (r.x + left_pad))
                        self.dirty = True
                        return True, "", "", False
                self.dragging_tf = False

            if (not in_player_view) and (not self.import_modal_open):
                new_hov = -1
                if self.mouse_y >= 72:
                    for i, (rect, _cat_idx) in enumerate(self.card_hits):
                        if rect.contains(self.mouse_x, self.mouse_y):
                            new_hov = i
                            break
                if new_hov != self.hovered_card:
                    self.hovered_card = new_hov
                self.dirty = True
                return True, "", "", False

            if self.import_modal_open or (in_player_view and self.mouse_y < 60):
                self.dirty = True
            return self.import_modal_open, "", "", False

        if etype == EVT_MOUSE_UP and ev.button == SDL_BUTTON_LEFT:
            if self.dragging_tf:
                self.dragging_tf = False
                tf = self._active_textfield()
                if tf and tf.sel_anchor == tf.cursor:
                    tf.sel_anchor = -1
                    self.dirty = True
                return True, "", "", False
            return False, "", "", False

        if etype == EVT_MOUSE_WHEEL:
            if self.import_modal_open:
                self.log_scroll_offset = max(0.0, self.log_scroll_offset + ev.wheel_y * 20.0)
                self.dirty = True
                return True, "", "", False
            if not in_player_view:
                self.scroll_y = max(0.0, min(self.max_scroll_y, self.scroll_y - ev.wheel_y * 42.0))
                self.dirty = True
                return True, "", "", False
            return False, "", "", False

        if etype == EVT_MOUSE_DOWN and ev.button == SDL_BUTTON_LEFT:
            mx = float(ev.x)
            my = float(ev.y)
            self.dragging_tf = False

            if self.import_modal_open:
                imp = self._read_import_snapshot()
                imp_st = imp["state"]
                imp_active = imp["active"]
                res_path = imp["result_disc_path"]
                res_title = imp["result_title"]

                if (
                    (not self.r_modal_box.contains(mx, my))
                    or self.r_modal_close.contains(mx, my)
                    or self.r_modal_done.contains(mx, my)
                ):
                    self.close_import_modal()
                    return True, "", "", False

                if self.r_modal_copy_log.contains(mx, my):
                    self.copy_import_log_to_clipboard()
                    return True, "", "", False

                if imp_active and self.r_modal_cancel.contains(mx, my):
                    self.lib.cancel_import()
                    self.dirty = True
                    return True, "", "", False

                if (not imp_active) and imp_st == IMPORT_DONE and self.r_modal_play.contains(mx, my) and res_path:
                    self.close_import_modal()
                    return True, res_path, res_title, False

                if not imp_active:
                    if self.r_modal_source.contains(mx, my):
                        self._handle_textfield_mouse_down(FOCUS_MODAL_SOURCE, ev.x, ev.clicks, ev.keymod)
                        return True, "", "", False
                    if self.r_modal_title.contains(mx, my):
                        self._handle_textfield_mouse_down(FOCUS_MODAL_TITLE, ev.x, ev.clicks, ev.keymod)
                        return True, "", "", False
                    if self.r_modal_browse.contains(mx, my):
                        chosen = choose_source_dialog(self.core, False, self.tf_source.text)
                        if chosen:
                            self.tf_source.set_text(chosen)
                        self.dirty = True
                        return True, "", "", False
                    if self.r_modal_browse_iso.contains(mx, my):
                        chosen = choose_source_dialog(self.core, True, self.tf_source.text)
                        if chosen:
                            self.tf_source.set_text(chosen)
                        self.dirty = True
                        return True, "", "", False
                    if self.r_modal_enrich.contains(mx, my):
                        self.enrich_checked = not self.enrich_checked
                        self.dirty = True
                        return True, "", "", False
                    if self.r_modal_start.contains(mx, my):
                        self.start_import_from_modal()
                        return True, "", "", False

                self.focus = FOCUS_NONE
                self.dirty = True
                return True, "", "", False

            if in_player_view:
                if self.last_hud_visible and self.r_back_lib.contains(mx, my):
                    return True, "", "", True
                return False, "", "", False

            # Library view clicks
            if self.r_open_import.contains(mx, my) or (
                (not self.items_snapshot) and self.r_empty_import.contains(mx, my)
            ):
                self.open_import_modal()
                return True, "", "", False

            if self.tf_search.text and self.r_search_clear.contains(mx, my):
                self.tf_search.set_text("")
                self.scroll_y = 0.0
                self.sync_catalog_snapshot()
                self.dirty = True
                return True, "", "", False

            if self.r_search.contains(mx, my):
                self._handle_textfield_mouse_down(FOCUS_SEARCH, ev.x, ev.clicks, ev.keymod)
                return True, "", "", False

            self.focus = FOCUS_NONE
            if my >= 72.0:
                for rect, cat_idx in self.card_hits:
                    if rect.contains(mx, my):
                        it = self.items_snapshot[cat_idx]
                        return True, it.path, it.title, False

            self.dirty = True
            return True, "", "", False

        if etype == EVT_TEXTINPUT:
            if ev.keymod & KMOD_GUI:
                return True, "", "", False
            txt = ev.text.decode("utf-8", errors="replace")
            tf = self._active_textfield()
            if tf:
                tf.insert_str(txt)
                if self.focus == FOCUS_SEARCH:
                    self.scroll_y = 0.0
                    self.sync_catalog_snapshot()
                self.dirty = True
                return True, "", "", False
            if (not in_player_view) and (not self.import_modal_open) and txt == "/":
                self.focus = FOCUS_SEARCH
                self.dirty = True
                return True, "", "", False
            return self.import_modal_open, "", "", False

        if etype == EVT_KEYDOWN:
            sym = ev.keysym
            sc = ev.scancode
            mod = ev.keymod
            accel = bool((mod & KMOD_GUI) or (mod & KMOD_CTRL))
            shift = bool(mod & KMOD_SHIFT)
            opt = bool(mod & KMOD_ALT)
            cmd = bool(mod & KMOD_GUI)

            is_a = (sym == SDLK_a) or (sc == SDL_SCANCODE_A)
            is_c = (sym == SDLK_c) or (sc == SDL_SCANCODE_C)
            is_v = (sym == SDLK_v) or (sc == SDL_SCANCODE_V)
            is_x = (sym == SDLK_x) or (sc == SDL_SCANCODE_X)
            is_f = (sym == SDLK_f) or (sc == SDL_SCANCODE_F)
            is_u = (sym == SDLK_u) or (sc == SDL_SCANCODE_U)

            tf = self._active_textfield()
            if accel and not tf:
                if (not in_player_view) and (not self.import_modal_open) and (is_v or is_a):
                    self.focus = FOCUS_SEARCH
                    tf = self.tf_search
                elif self.import_modal_open and is_c:
                    self.copy_import_log_to_clipboard()
                    return True, "", "", False

            if tf:
                if accel and is_a:
                    tf.select_all()
                    self.dirty = True
                    return True, "", "", False
                if accel and is_c:
                    tf.copy_selection(self.core)
                    return True, "", "", False
                if accel and is_x:
                    if tf.cut_selection(self.core):
                        if self.focus == FOCUS_SEARCH:
                            self.scroll_y = 0.0
                            self.sync_catalog_snapshot()
                        self.dirty = True
                    return True, "", "", False
                if accel and is_v:
                    ptr = self.core.lib.tdvd_desktop_get_clipboard()
                    if ptr:
                        try:
                            clip = ctypes.string_at(ptr).decode("utf-8", errors="replace")
                        finally:
                            self.core.lib.tdvd_free(ptr)
                        tf.insert_str(clip)
                        if self.focus == FOCUS_SEARCH:
                            self.scroll_y = 0.0
                            self.sync_catalog_snapshot()
                        self.dirty = True
                    return True, "", "", False
                if accel and is_f and (not self.import_modal_open) and (not in_player_view):
                    self.focus = FOCUS_SEARCH
                    self.tf_search.select_all()
                    self.dirty = True
                    return True, "", "", False
                if bool(mod & KMOD_CTRL) and is_u:
                    tf.set_text("")
                    if self.focus == FOCUS_SEARCH:
                        self.scroll_y = 0.0
                        self.sync_catalog_snapshot()
                    self.dirty = True
                    return True, "", "", False
                if sym == SDLK_BACKSPACE:
                    tf.backspace(opt, cmd)
                    if self.focus == FOCUS_SEARCH:
                        self.scroll_y = 0.0
                        self.sync_catalog_snapshot()
                    self.dirty = True
                    return True, "", "", False
                if sym == SDLK_DELETE:
                    tf.delete_fwd(opt)
                    if self.focus == FOCUS_SEARCH:
                        self.scroll_y = 0.0
                        self.sync_catalog_snapshot()
                    self.dirty = True
                    return True, "", "", False
                if sym == SDLK_LEFT:
                    if (not shift) and (not cmd) and (not opt) and tf.has_selection():
                        s, _e = tf.get_selection()
                        tf.move_cursor(s, False)
                    else:
                        next_pos = 0 if cmd else (tf.prev_word_pos(tf.cursor) if opt else (tf.cursor - 1))
                        tf.move_cursor(next_pos, shift)
                    self.dirty = True
                    return True, "", "", False
                if sym == SDLK_RIGHT:
                    if (not shift) and (not cmd) and (not opt) and tf.has_selection():
                        _s, e = tf.get_selection()
                        tf.move_cursor(e, False)
                    else:
                        next_pos = len(tf.text) if cmd else (tf.next_word_pos(tf.cursor) if opt else (tf.cursor + 1))
                        tf.move_cursor(next_pos, shift)
                    self.dirty = True
                    return True, "", "", False
                if sym in (SDLK_HOME, SDLK_UP):
                    tf.move_cursor(0, shift)
                    self.dirty = True
                    return True, "", "", False
                if sym in (SDLK_END, SDLK_DOWN):
                    tf.move_cursor(len(tf.text), shift)
                    self.dirty = True
                    return True, "", "", False
                if sym == SDLK_TAB and self.import_modal_open:
                    self.dragging_tf = False
                    self.focus = FOCUS_MODAL_TITLE if self.focus == FOCUS_MODAL_SOURCE else FOCUS_MODAL_SOURCE
                    self.dirty = True
                    return True, "", "", False
                if sym in (SDLK_RETURN, SDLK_KP_ENTER):
                    if self.import_modal_open:
                        self.start_import_from_modal()
                        return True, "", "", False
                    if self.focus == FOCUS_SEARCH and self.filtered_indices:
                        idx = self.selected_filtered if 0 <= self.selected_filtered < len(self.filtered_indices) else 0
                        it = self.items_snapshot[self.filtered_indices[idx]]
                        return True, it.path, it.title, False
                    return True, "", "", False
                if sym == SDLK_ESCAPE:
                    if self.import_modal_open:
                        self.close_import_modal()
                    elif self.focus == FOCUS_SEARCH and self.tf_search.text:
                        self.tf_search.set_text("")
                        self.scroll_y = 0.0
                        self.sync_catalog_snapshot()
                        self.dirty = True
                    else:
                        self.focus = FOCUS_NONE
                        self.dirty = True
                    return True, "", "", False
                return True, "", "", False

            if self.import_modal_open:
                if sym == SDLK_ESCAPE:
                    self.close_import_modal()
                elif sym == SDLK_TAB:
                    self.focus = FOCUS_MODAL_SOURCE
                    self.dirty = True
                return True, "", "", False

            if not in_player_view:
                if accel and is_f:
                    self.focus = FOCUS_SEARCH
                    self.tf_search.select_all()
                    self.dirty = True
                    return True, "", "", False
                if sym in (SDLK_DOWN, SDLK_PAGEDOWN):
                    step = 240.0 if sym == SDLK_PAGEDOWN else 60.0
                    self.scroll_y = min(self.max_scroll_y, self.scroll_y + step)
                    self.dirty = True
                    return True, "", "", False
                if sym in (SDLK_UP, SDLK_PAGEUP):
                    step = 240.0 if sym == SDLK_PAGEUP else 60.0
                    self.scroll_y = max(0.0, self.scroll_y - step)
                    self.dirty = True
                    return True, "", "", False
                if sym in (SDLK_RETURN, SDLK_KP_ENTER) and self.filtered_indices:
                    idx = self.selected_filtered if 0 <= self.selected_filtered < len(self.filtered_indices) else 0
                    it = self.items_snapshot[self.filtered_indices[idx]]
                    return True, it.path, it.title, False
                if sym in (SDLK_q, SDLK_ESCAPE, SDLK_f):
                    return False, "", "", False
                return True, "", "", False

        return False, "", "", False


def run_desktop_app(core: TinyDVDCore, dvd_path: str, start_fullscreen: bool) -> int:
    """Run the TinyDVD Cinema desktop window, library browser, and player loop."""
    catalog_changed_event = threading.Event()
    import_changed_event = threading.Event()

    lib = TinyDVDLibrary(
        dvd_path,
        on_catalog_change=catalog_changed_event.set,
        on_import_change=import_changed_event.set,
    )
    is_library, init_items, target_path = lib.get_snapshot()

    fallback_dir = ""
    if not is_library:
        fallback_dir = os.path.dirname(target_path) or "."

    init_w, init_h = (1280, 800) if is_library else (853, 480)
    init_title = "TinyDVD Cinema" if is_library else "tinydvd Player"

    ctx = core.lib.tdvd_desktop_create(
        init_title.encode("utf-8"),
        init_w,
        init_h,
        1 if start_fullscreen else 0,
    )
    if not ctx:
        lib.destroy()
        return 1

    ui = TinyDVDDesktopUI(core, ctx, lib)
    in_player_view = False
    current_movie_title = ""
    is_fullscreen = bool(start_fullscreen)
    cursor_visible = True
    last_mouse_activity = int(core.lib.tdvd_desktop_get_ticks())

    def start_movie(disc_path: str, disc_title: str) -> bool:
        nonlocal in_player_view, current_movie_title, last_mouse_activity
        disp = disc_title or disc_path
        wtitle = f"TinyDVD — {disp}"
        ok = core.lib.tdvd_desktop_start_movie(
            ctx, disc_path.encode("utf-8"), wtitle.encode("utf-8")
        )
        if not ok:
            return False
        in_player_view = True
        current_movie_title = disp
        last_mouse_activity = int(core.lib.tdvd_desktop_get_ticks())
        ui.mark_dirty()
        return True

    def back_to_library() -> None:
        nonlocal in_player_view, current_movie_title, cursor_visible, is_library
        core.lib.tdvd_desktop_stop_movie(ctx)
        lib_mode, _, _ = lib.get_snapshot()
        if (not lib_mode) and fallback_dir:
            lib.set_directory(fallback_dir)
        else:
            lib.rescan()
        core.lib.tdvd_desktop_set_cursor_visible(ctx, 1)
        cursor_visible = True
        in_player_view = False
        current_movie_title = ""
        is_library = True
        core.lib.tdvd_desktop_set_window_title(ctx, b"TinyDVD Cinema")
        ui.mark_dirty()

    try:
        if not is_library:
            play_target = init_items[0].path if init_items else target_path
            play_title = init_items[0].title if init_items else dvd_path
            if not start_movie(play_target, play_title):
                return 1

        ev = DesktopUIEvent()
        ww_c = ctypes.c_int(0)
        wh_c = ctypes.c_int(0)
        dw_c = ctypes.c_int(0)
        dh_c = ctypes.c_int(0)

        running = True
        while running and not core.lib.tdvd_desktop_quit_requested():
            if in_player_view and not core.lib.tdvd_desktop_is_vm_running(ctx):
                back_to_library()

            core.lib.tdvd_desktop_get_sizes(
                ctx, ctypes.byref(ww_c), ctypes.byref(wh_c), ctypes.byref(dw_c), ctypes.byref(dh_c)
            )
            win_w, win_h, draw_w, draw_h = ww_c.value, wh_c.value, dw_c.value, dh_c.value

            while core.lib.tdvd_desktop_poll_event(ctx, ctypes.byref(ev)):
                if ev.type in (EVT_MOUSE_MOTION, EVT_MOUSE_DOWN, EVT_MOUSE_UP, EVT_MOUSE_WHEEL):
                    last_mouse_activity = int(core.lib.tdvd_desktop_get_ticks())
                    if not cursor_visible:
                        core.lib.tdvd_desktop_set_cursor_visible(ctx, 1)
                        cursor_visible = True
                        ui.mark_dirty()

                if ev.type == EVT_QUIT:
                    running = False
                    break

                if ev.type == EVT_FILE_OPENED:
                    path_buf = ctypes.create_string_buffer(4096)
                    if core.lib.tdvd_desktop_take_dropped_file(ctx, path_buf, len(path_buf)):
                        opened = path_buf.value.decode("utf-8", errors="surrogateescape")
                        if (tinydvd_archive.is_tdvd_path(opened) and os.path.isfile(opened)) or is_dvd_dir(opened):
                            title = str(tinydvd_archive.read_metadata(opened).get("title") or "").strip()
                            start_movie(opened, title or tinydvd_archive.strip_tdvd_extension(
                                os.path.basename(opened.rstrip("/"))))
                    continue

                consumed, play_path, play_title, do_back = ui.handle_event(
                    ev, win_w, win_h, in_player_view
                )
                if consumed:
                    if play_path:
                        start_movie(play_path, play_title)
                    elif do_back and in_player_view:
                        back_to_library()
                    continue

                if ev.type == EVT_KEYDOWN:
                    sym = ev.keysym
                    mod = ev.keymod
                    action = player_key_action(sym, mod) if in_player_view else None
                    if sym == SDLK_q:
                        running = False
                    elif action:
                        core.lib.tdvd_desktop_vm_action(ctx, action[0], 0, 0, action[1])
                    elif sym == SDLK_ESCAPE:
                        if in_player_view:
                            if not core.lib.tdvd_desktop_vm_action(ctx, b"escape", 0, 0, 0.0):
                                back_to_library()
                        else:
                            running = False
                    elif sym == SDLK_f:
                        is_fullscreen = not is_fullscreen
                        core.lib.tdvd_desktop_set_fullscreen(ctx, 1 if is_fullscreen else 0)
                        ui.mark_dirty()
                elif ev.type == EVT_MOUSE_MOTION and in_player_view:
                    core.lib.tdvd_desktop_vm_action(ctx, b"mouse_move", ev.x, ev.y, 0.0)
                elif ev.type == EVT_MOUSE_DOWN and in_player_view and ev.button == SDL_BUTTON_LEFT:
                    core.lib.tdvd_desktop_vm_action(ctx, b"mouse_click", ev.x, ev.y, 0.0)

            ticks = int(core.lib.tdvd_desktop_get_ticks())
            if in_player_view and (not ui.import_modal_open):
                if cursor_visible and (ticks - last_mouse_activity >= CURSOR_IDLE_MS):
                    core.lib.tdvd_desktop_set_cursor_visible(ctx, 0)
                    cursor_visible = False
                    ui.mark_dirty()
            elif not cursor_visible:
                core.lib.tdvd_desktop_set_cursor_visible(ctx, 1)
                cursor_visible = True
                ui.mark_dirty()

            if catalog_changed_event.is_set():
                catalog_changed_event.clear()
                ui.mark_dirty()

            hud_vis = cursor_visible or ui.import_modal_open
            ui.render_if_needed(
                win_w,
                win_h,
                draw_w,
                draw_h,
                in_player_view,
                current_movie_title,
                hud_vis,
                import_changed_event,
            )

            draw_overlay = (not in_player_view) or hud_vis
            core.lib.tdvd_desktop_present_frame(ctx, 1 if in_player_view else 0, 1 if draw_overlay else 0)

        return 0
    finally:
        lib.destroy()
        core.lib.tdvd_desktop_destroy(ctx)

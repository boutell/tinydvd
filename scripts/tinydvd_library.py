#!/usr/bin/env python3
"""
Pure-Python library catalog, .tdvd archive reader, OS-native directory watcher
(kqueue on macOS, inotify on Linux), optical drive detector, and DVD import job manager.
"""

from __future__ import annotations

from dataclasses import dataclass, asdict
import ctypes
import ctypes.util
import hashlib
import json
import os
import re
import select
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile


IMPORT_IDLE = 0
IMPORT_RIPPING = 1
IMPORT_CONVERTING = 2
IMPORT_DONE = 3
IMPORT_ERROR = 4

IMPORT_MAX_LOGS = 64

IMPORT_STATE_NAMES = {
    IMPORT_IDLE: "idle",
    IMPORT_RIPPING: "ripping",
    IMPORT_CONVERTING: "converting",
    IMPORT_DONE: "done",
    IMPORT_ERROR: "error",
}


def sanitize_id(name: str) -> str:
    if not name:
        return ""
    return "".join(c if (c.isalnum() or c in "-_") else "_" for c in name)[:127]


def is_tdvd_file(path: str) -> bool:
    if not path:
        return False
    low = path.lower()
    return low.endswith(".tdvd") or low.endswith(".tinydvd")


def is_dvd_dir(dir_path: str) -> bool:
    if not dir_path:
        return False
    return (
        os.path.exists(os.path.join(dir_path, "VIDEO_TS.IFO"))
        or os.path.exists(os.path.join(dir_path, "VIDEO_TS", "VIDEO_TS.IFO"))
    )


def format_size(size_bytes: int) -> str:
    if size_bytes >= 1024 * 1024 * 1024:
        return f"{size_bytes / (1024.0 * 1024.0 * 1024.0):.1f} GB"
    if size_bytes >= 1024 * 1024:
        return f"{size_bytes / (1024.0 * 1024.0):.1f} MB"
    if size_bytes >= 1024:
        return f"{size_bytes / 1024.0:.0f} KB"
    return f"{size_bytes} B"


def _flatten_meta_field(val) -> str:
    if isinstance(val, list):
        return ", ".join(str(x).strip() for x in val if str(x).strip())
    if val is None:
        return ""
    return str(val).strip()


def _find_zip_member(zf: zipfile.ZipFile, target_basename: str):
    target_low = target_basename.lower()
    for info in zf.infolist():
        if os.path.basename(info.filename).lower() == target_low:
            return info
    return None


@dataclass
class CatalogItem:
    id: str
    title: str
    path: str
    size_bytes: int
    size_str: str
    has_thumbnail: bool
    year: int = 0
    director: str = ""
    cast: str = ""
    genres: str = ""
    overview: str = ""

    def to_api_dict(self) -> dict:
        return {
            "id": self.id,
            "title": self.title,
            "size_bytes": self.size_bytes,
            "size_str": self.size_str,
            "has_thumbnail": self.has_thumbnail,
            "year": self.year,
            "director": self.director,
            "cast": self.cast,
            "genres": self.genres,
            "overview": self.overview,
        }


def _clean_disc_label(raw_label: str) -> str:
    s = raw_label.replace("_", " ").strip()
    if not s:
        return ""
    if s.isupper():
        s = s.title()
    return s


def detect_default_source() -> tuple[str, str]:
    """Auto-detect a mounted DVD volume or optical device and a clean title hint."""
    candidates: list[tuple[str, str]] = []
    if sys.platform == "darwin":
        volumes_dir = "/Volumes"
        if os.path.isdir(volumes_dir):
            try:
                for entry in sorted(os.listdir(volumes_dir)):
                    if entry.startswith("."):
                        continue
                    vp = os.path.join(volumes_dir, entry)
                    if os.path.isdir(vp) and is_dvd_dir(vp):
                        candidates.append((vp, _clean_disc_label(entry)))
            except OSError:
                pass
    else:
        for base_dir in ("/media", "/run/media", "/mnt"):
            if not os.path.isdir(base_dir):
                continue
            try:
                for root, dirs, _ in os.walk(base_dir):
                    depth = root[len(base_dir):].count(os.sep)
                    if depth > 2:
                        dirs[:] = []
                        continue
                    if is_dvd_dir(root):
                        candidates.append((root, _clean_disc_label(os.path.basename(root))))
                        dirs[:] = []
            except OSError:
                pass
        for dev in ("/dev/sr0", "/dev/cdrom", "/dev/dvd"):
            if os.path.exists(dev):
                candidates.append((dev, ""))

    if candidates:
        return candidates[0]
    return ("", "")


def _clean_output_stem(raw: str) -> str:
    stem = re.sub(r'[<>:"/\\|?*\x00-\x1f]+', " ", (raw or "").strip())
    stem = re.sub(r"\s+", " ", stem).strip(" .")
    return stem or "Imported DVD"


def _compute_dir_signature(target_path: str, is_library: bool) -> int:
    if not target_path:
        return 0
    try:
        st = os.stat(target_path)
    except OSError:
        return 0xDEADBEEF00000001

    h = hashlib.blake2b(digest_size=8)
    h.update(struct.pack("<QQI", st.st_dev, st.st_ino, st.st_mode))
    if is_library and stat.S_ISDIR(st.st_mode):
        entry_hashes = []
        try:
            with os.scandir(target_path) as it:
                for entry in it:
                    if entry.name.startswith("."):
                        continue
                    try:
                        est = entry.stat(follow_symlinks=True)
                    except OSError:
                        continue
                    eh = hashlib.blake2b(digest_size=8)
                    eh.update(entry.name.encode("utf-8", errors="surrogateescape"))
                    eh.update(
                        struct.pack(
                            "<IQQdd",
                            est.st_mode,
                            est.st_ino,
                            est.st_size,
                            est.st_mtime,
                            est.st_ctime,
                        )
                    )
                    entry_hashes.append(eh.digest())
        except OSError:
            pass
        entry_hashes.sort()
        h.update(struct.pack("<Q", len(entry_hashes)))
        for eh in entry_hashes:
            h.update(eh)
    return int.from_bytes(h.digest(), "little")


def _load_item(path: str, title_override: str | None = None) -> CatalogItem:
    base = os.path.basename(path.rstrip("/")) or path
    item_id = sanitize_id(base)

    if title_override and title_override.strip():
        title = title_override.strip()[:255]
    else:
        title = base
        for ext in (".tdvd", ".tinydvd"):
            if title.lower().endswith(ext):
                title = title[: -len(ext)]
                break

    size_bytes = 0
    try:
        size_bytes = os.path.getsize(path)
    except OSError:
        pass
    size_str = format_size(size_bytes)

    has_thumbnail = False
    year = 0
    director = ""
    cast = ""
    genres = ""
    overview = ""

    def apply_meta(m: dict) -> None:
        nonlocal title, year, director, cast, genres, overview
        if not isinstance(m, dict):
            return
        t = str(m.get("title") or "").strip()
        if t:
            title = t[:255]
        try:
            y = int(m.get("year") or 0)
            if y > 0:
                year = y
        except (TypeError, ValueError):
            pass
        if not director:
            director = _flatten_meta_field(m.get("director"))[:127]
        if not cast:
            cast = _flatten_meta_field(m.get("cast"))[:1023]
        if not genres:
            genres = _flatten_meta_field(m.get("genres"))[:127]
        if not overview:
            overview = _flatten_meta_field(m.get("overview"))[:1023]

    if is_tdvd_file(path) and zipfile.is_zipfile(path):
        try:
            with zipfile.ZipFile(path, "r") as zf:
                t_info = _find_zip_member(zf, "thumbnail.png")
                has_thumbnail = t_info is not None and t_info.file_size > 0
                m_info = _find_zip_member(zf, "metadata.json")
                if m_info and 0 < m_info.file_size < 65536:
                    meta = json.loads(zf.read(m_info).decode("utf-8", errors="replace"))
                    apply_meta(meta)
        except Exception:
            pass
    elif os.path.isdir(path):
        tpath = os.path.join(path, "thumbnail.png")
        try:
            has_thumbnail = os.path.isfile(tpath) and os.path.getsize(tpath) > 0
        except OSError:
            pass

    if not director:
        if os.path.isdir(path):
            sidecar = os.path.join(path, "metadata.json")
        else:
            root_no_ext, _ = os.path.splitext(path)
            sidecar = root_no_ext + ".json"
        if os.path.isfile(sidecar):
            try:
                if 0 < os.path.getsize(sidecar) < 65536:
                    with open(sidecar, "r", encoding="utf-8", errors="replace") as sf:
                        apply_meta(json.load(sf))
            except Exception:
                pass

    return CatalogItem(
        id=item_id,
        title=title,
        path=path,
        size_bytes=size_bytes,
        size_str=size_str,
        has_thumbnail=has_thumbnail,
        year=year,
        director=director,
        cast=cast,
        genres=genres,
        overview=overview,
    )


class TinyDVDLibrary:
    """Thread-safe library catalog, archive reader, OS watcher, and import runner."""

    def __init__(self, target_path: str, on_catalog_change=None, on_import_change=None):
        self.catalog_lock = threading.Lock()
        self.catalog_cond = threading.Condition(self.catalog_lock)
        self.import_lock = threading.Lock()

        self.on_catalog_change = on_catalog_change
        self.on_import_change = on_import_change

        expanded = self._expand_path(target_path)
        self.target_path = expanded
        self.is_library = os.path.isdir(expanded) and not is_dvd_dir(expanded)

        self.catalog: list[CatalogItem] = []
        self.dir_signature: int = 0
        self.catalog_generation: int = 0
        self.default_disc_path: str = ""
        self.default_disc_id: str = ""
        self.default_disc_title: str = ""

        # Import state
        self.imp_state: int = IMPORT_IDLE
        self.imp_active: bool = False
        self.imp_cancel_requested: bool = False
        self.imp_proc: subprocess.Popen | None = None
        self.imp_thread: threading.Thread | None = None
        self.imp_start_time: int = 0
        self.imp_source: str = ""
        self.imp_title: str = ""
        self.imp_skip_enrichment: bool = False
        self.imp_stage_msg: str = ""
        self.imp_error_msg: str = ""
        self.imp_result_disc_id: str = ""
        self.imp_result_disc_path: str = ""
        self.imp_result_title: str = ""
        self.imp_logs: list[str] = []

        self.rescan()

        self._watch_stop = False
        self._wake_r, self._wake_w = os.pipe()
        os.set_blocking(self._wake_r, False)
        os.set_blocking(self._wake_w, False)
        self._watch_thread = threading.Thread(
            target=self._watch_loop, name="tdvd-dir-watch", daemon=True
        )
        self._watch_thread.start()

    @staticmethod
    def _expand_path(path: str) -> str:
        p = os.path.expanduser(path or "")
        if len(p) > 1:
            p = p.rstrip("/")
        return p or "."

    def _wake_watcher(self, byte: bytes = b"w") -> None:
        if self._wake_w >= 0:
            try:
                os.write(self._wake_w, byte)
            except OSError:
                pass

    def wake_sse_clients(self) -> None:
        with self.catalog_cond:
            self.catalog_cond.notify_all()

    def get_generation(self) -> int:
        with self.catalog_lock:
            return self.catalog_generation

    def get_snapshot(self) -> tuple[bool, list[CatalogItem], str]:
        with self.catalog_lock:
            return self.is_library, list(self.catalog), self.target_path

    def find_by_id(self, disc_id: str) -> CatalogItem | None:
        with self.catalog_lock:
            if not disc_id:
                return self.catalog[0] if self.catalog else None
            for it in self.catalog:
                if it.id == disc_id:
                    return it
            return None

    def set_directory(self, dir_path: str) -> None:
        if not dir_path:
            return
        expanded = self._expand_path(dir_path)
        with self.catalog_lock:
            self.target_path = expanded
            self.is_library = True
        self.rescan()
        self._wake_watcher(b"w")

    def rescan(self) -> None:
        with self.catalog_lock:
            tpath = self.target_path
            is_lib = self.is_library

        items: list[CatalogItem] = []
        if os.path.isdir(tpath):
            if not is_lib and is_dvd_dir(tpath):
                items.append(_load_item(tpath))
            else:
                is_lib = True
                try:
                    with os.scandir(tpath) as it:
                        for entry in it:
                            if entry.name.startswith("."):
                                continue
                            full = entry.path
                            try:
                                if entry.is_file(follow_symlinks=True):
                                    if is_tdvd_file(entry.name):
                                        items.append(_load_item(full))
                                elif entry.is_dir(follow_symlinks=True):
                                    if is_dvd_dir(full):
                                        items.append(_load_item(full))
                            except OSError:
                                continue
                except OSError:
                    pass
        else:
            if not is_lib and os.path.exists(tpath):
                items.append(_load_item(tpath))

        items.sort(key=lambda c: (c.title.lower(), c.year, c.id))
        new_sig = _compute_dir_signature(tpath, is_lib)

        changed_cb = None
        with self.catalog_cond:
            self.is_library = is_lib
            self.catalog = items
            if items:
                self.default_disc_path = items[0].path
                self.default_disc_id = items[0].id
                self.default_disc_title = items[0].title
            else:
                self.default_disc_path = ""
                self.default_disc_id = ""
                self.default_disc_title = ""

            if new_sig != self.dir_signature or self.catalog_generation == 0:
                self.dir_signature = new_sig
                self.catalog_generation += 1
                self.catalog_cond.notify_all()
                changed_cb = self.on_catalog_change

        if changed_cb:
            try:
                changed_cb()
            except Exception:
                pass

    def check_for_changes(self) -> bool:
        with self.catalog_lock:
            if not self.is_library:
                return False
            tpath = self.target_path
            prev_sig = self.dir_signature

        cur_sig = _compute_dir_signature(tpath, True)
        if cur_sig != prev_sig:
            self.rescan()
            return True
        return False

    def _drain_fd(self, fd: int) -> None:
        while True:
            try:
                data = os.read(fd, 1024)
                if not data:
                    break
            except BlockingIOError:
                break
            except OSError:
                break

    def _coalesce(self, delay_sec: float = 0.10) -> bool:
        r, _, _ = select.select([self._wake_r], [], [], delay_sec)
        if r:
            self._drain_fd(self._wake_r)
        return not self._watch_stop

    def _watch_loop(self) -> None:
        if hasattr(select, "kqueue"):
            self._watch_loop_kqueue()
        elif sys.platform.startswith("linux"):
            self._watch_loop_linux()
        else:
            self._watch_loop_fallback()

    def _watch_loop_kqueue(self) -> None:
        kq = select.kqueue()
        kq.control(
            [
                select.kevent(
                    self._wake_r,
                    filter=select.KQ_FILTER_READ,
                    flags=select.KQ_EV_ADD | select.KQ_EV_ENABLE | select.KQ_EV_CLEAR,
                )
            ],
            0,
            0,
        )

        watched_fd = -1
        watched_path = ""
        o_evtonly = getattr(os, "O_EVTONLY", os.O_RDONLY)
        vnode_mask = (
            select.KQ_NOTE_WRITE
            | select.KQ_NOTE_EXTEND
            | select.KQ_NOTE_ATTRIB
            | select.KQ_NOTE_DELETE
            | select.KQ_NOTE_RENAME
            | getattr(select, "KQ_NOTE_LINK", 0)
        )

        try:
            while not self._watch_stop:
                with self.catalog_lock:
                    is_lib = self.is_library
                    tpath = self.target_path
                if self._watch_stop:
                    break

                if not is_lib or watched_fd < 0 or tpath != watched_path:
                    if watched_fd >= 0:
                        os.close(watched_fd)
                        watched_fd = -1
                        watched_path = ""
                    if is_lib and tpath:
                        try:
                            fd = os.open(tpath, o_evtonly)
                            try:
                                kq.control(
                                    [
                                        select.kevent(
                                            fd,
                                            filter=select.KQ_FILTER_VNODE,
                                            flags=select.KQ_EV_ADD | select.KQ_EV_ENABLE | select.KQ_EV_CLEAR,
                                            fflags=vnode_mask,
                                        )
                                    ],
                                    0,
                                    0,
                                )
                                watched_fd = fd
                                watched_path = tpath
                            except Exception:
                                os.close(fd)
                        except OSError:
                            pass

                try:
                    events = kq.control(None, 4, None)
                except InterruptedError:
                    continue
                except OSError:
                    break

                reopen = False
                fs_event = False
                for ev in events:
                    if ev.ident == self._wake_r:
                        self._drain_fd(self._wake_r)
                        reopen = True
                    elif watched_fd >= 0 and ev.ident == watched_fd:
                        fs_event = True
                        if ev.fflags & (select.KQ_NOTE_DELETE | select.KQ_NOTE_RENAME):
                            reopen = True

                if self._watch_stop:
                    break
                if reopen and watched_fd >= 0:
                    os.close(watched_fd)
                    watched_fd = -1
                    watched_path = ""
                if fs_event:
                    if not self._coalesce(0.10):
                        break
                    self.check_for_changes()
        finally:
            if watched_fd >= 0:
                os.close(watched_fd)
            kq.close()

    def _watch_loop_linux(self) -> None:
        libc_name = ctypes.util.find_library("c")
        libc = ctypes.CDLL(libc_name, use_errno=True) if libc_name else None
        in_fd = -1
        if libc and hasattr(libc, "inotify_init1") and hasattr(libc, "inotify_add_watch"):
            libc.inotify_init1.argtypes = [ctypes.c_int]
            libc.inotify_init1.restype = ctypes.c_int
            libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
            libc.inotify_add_watch.restype = ctypes.c_int
            libc.inotify_rm_watch.argtypes = [ctypes.c_int, ctypes.c_int]
            libc.inotify_rm_watch.restype = ctypes.c_int
            in_fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)

        if in_fd < 0:
            self._watch_loop_fallback()
            return

        # IN_MODIFY(2)|IN_ATTRIB(4)|IN_CLOSE_WRITE(8)|IN_MOVED_FROM(64)|IN_MOVED_TO(128)|IN_CREATE(256)|IN_DELETE(512)|IN_DELETE_SELF(1024)|IN_MOVE_SELF(2048)
        in_mask = 2 | 4 | 8 | 64 | 128 | 256 | 512 | 1024 | 2048
        wd = -1
        watched_path = ""
        poller = select.poll()
        poller.register(self._wake_r, select.POLLIN)
        poller.register(in_fd, select.POLLIN)

        try:
            while not self._watch_stop:
                with self.catalog_lock:
                    is_lib = self.is_library
                    tpath = self.target_path
                if self._watch_stop:
                    break

                if not is_lib or wd < 0 or tpath != watched_path:
                    if wd >= 0:
                        libc.inotify_rm_watch(in_fd, wd)
                        wd = -1
                        watched_path = ""
                    if is_lib and tpath:
                        new_wd = libc.inotify_add_watch(
                            in_fd, tpath.encode("utf-8", errors="surrogateescape"), in_mask
                        )
                        if new_wd >= 0:
                            wd = new_wd
                            watched_path = tpath

                try:
                    ready = poller.poll(-1)
                except InterruptedError:
                    continue

                fs_event = False
                for fd, _ in ready:
                    if fd == self._wake_r:
                        self._drain_fd(self._wake_r)
                        if wd >= 0:
                            libc.inotify_rm_watch(in_fd, wd)
                            wd = -1
                            watched_path = ""
                    elif fd == in_fd:
                        self._drain_fd(in_fd)
                        fs_event = True

                if self._watch_stop:
                    break
                if fs_event:
                    if not self._coalesce(0.10):
                        break
                    self.check_for_changes()
        finally:
            if wd >= 0:
                libc.inotify_rm_watch(in_fd, wd)
            os.close(in_fd)

    def _watch_loop_fallback(self) -> None:
        while not self._watch_stop:
            r, _, _ = select.select([self._wake_r], [], [], 2.0)
            if r:
                self._drain_fd(self._wake_r)
                continue
            if self._watch_stop:
                break
            self.check_for_changes()

    def read_thumbnail(self, disc_path: str) -> bytes | None:
        if not disc_path:
            return None
        if is_tdvd_file(disc_path) and zipfile.is_zipfile(disc_path):
            try:
                with zipfile.ZipFile(disc_path, "r") as zf:
                    info = _find_zip_member(zf, "thumbnail.png")
                    if info and 0 < info.file_size < 16 * 1024 * 1024:
                        return zf.read(info)
            except Exception:
                return None
        if os.path.isdir(disc_path):
            tpath = os.path.join(disc_path, "thumbnail.png")
            try:
                if os.path.isfile(tpath) and 0 < os.path.getsize(tpath) < 16 * 1024 * 1024:
                    with open(tpath, "rb") as f:
                        return f.read()
            except OSError:
                return None
        return None

    def get_mkv_slice(self, disc_path: str, mkv_name: str):
        """
        Locate an embedded .mkv inside a .tdvd archive (returning raw byte range
        or zip entry) or inside an unpacked directory.
        """
        if not disc_path or not mkv_name:
            return None
        if is_tdvd_file(disc_path) and zipfile.is_zipfile(disc_path):
            try:
                with zipfile.ZipFile(disc_path, "r") as zf, open(disc_path, "rb") as raw:
                    info = _find_zip_member(zf, mkv_name)
                    if not info:
                        return None
                    if info.compress_type == zipfile.ZIP_STORED:
                        raw.seek(info.header_offset)
                        hdr = raw.read(30)
                        if len(hdr) < 30:
                            return None
                        name_len, extra_len = struct.unpack("<HH", hdr[26:30])
                        data_offset = info.header_offset + 30 + name_len + extra_len
                        return ("slice", disc_path, data_offset, info.file_size)
                    return ("zip", disc_path, info.filename, info.file_size)
            except Exception:
                return None
        if os.path.isdir(disc_path):
            cand = os.path.join(disc_path, mkv_name)
            if os.path.isfile(cand):
                return ("file", cand, 0, os.path.getsize(cand))
            low = mkv_name.lower()
            try:
                for name in os.listdir(disc_path):
                    if name.lower() == low:
                        full = os.path.join(disc_path, name)
                        if os.path.isfile(full):
                            return ("file", full, 0, os.path.getsize(full))
            except OSError:
                pass
        return None

    def _notify_import_change(self) -> None:
        if self.on_import_change:
            try:
                self.on_import_change()
            except Exception:
                pass

    def _append_log(self, line: str) -> None:
        clean = (line or "").rstrip("\r\n")
        if not clean:
            return
        with self.import_lock:
            self.imp_logs.append(clean[:255])
            if len(self.imp_logs) > IMPORT_MAX_LOGS:
                self.imp_logs = self.imp_logs[-IMPORT_MAX_LOGS:]

    def get_import_status(self) -> dict:
        def_src, def_title = detect_default_source()
        with self.import_lock:
            elapsed = int(time.time() - self.imp_start_time) if self.imp_start_time > 0 else 0
            return {
                "state_code": self.imp_state,
                "state": IMPORT_STATE_NAMES.get(self.imp_state, "idle"),
                "active": self.imp_active,
                "elapsed_sec": elapsed,
                "start_time": self.imp_start_time,
                "default_source": def_src,
                "default_title": def_title,
                "source": self.imp_source,
                "title": self.imp_title,
                "stage_msg": self.imp_stage_msg,
                "error_msg": self.imp_error_msg,
                "result_disc_id": self.imp_result_disc_id,
                "result_disc_path": self.imp_result_disc_path,
                "result_title": self.imp_result_title,
                "logs": list(self.imp_logs),
            }

    def start_import(self, source: str, title: str = "", skip_enrichment: bool = False) -> tuple[bool, str]:
        src = (source or "").strip()
        if not src:
            return False, "Missing DVD source path."

        scripts_dir = os.path.dirname(os.path.abspath(__file__))
        convert_script = os.path.join(scripts_dir, "convert-to-tinydvd.py")
        if not os.path.isfile(convert_script):
            return False, "convert-to-tinydvd.py not found."

        with self.catalog_lock:
            out_dir = self.target_path if self.is_library else (os.path.dirname(self.target_path) or ".")

        if not os.path.isdir(out_dir):
            try:
                os.makedirs(out_dir, exist_ok=True)
            except OSError as e:
                return False, f"Cannot create library directory: {e}"

        title_clean = (title or "").strip()
        if title_clean:
            stem = _clean_output_stem(title_clean)
        else:
            b = os.path.basename(src.rstrip("/"))
            for ext in (".iso", ".img", ".nrg", ".tdvd", ".tinydvd"):
                if b.lower().endswith(ext):
                    b = b[: -len(ext)]
                    break
            stem = _clean_output_stem(_clean_disc_label(b) if b.isupper() else b)

        desired_output = os.path.join(out_dir, f"{stem}.tdvd")

        with self.import_lock:
            if self.imp_active:
                return False, "An import job is already in progress."
            self.imp_state = IMPORT_CONVERTING
            self.imp_active = True
            self.imp_cancel_requested = False
            self.imp_proc = None
            self.imp_start_time = int(time.time())
            self.imp_source = src
            self.imp_title = title_clean
            self.imp_skip_enrichment = bool(skip_enrichment)
            self.imp_stage_msg = "Starting import..."
            self.imp_error_msg = ""
            self.imp_result_disc_id = ""
            self.imp_result_disc_path = ""
            self.imp_result_title = ""
            self.imp_logs = []

        self._notify_import_change()
        t = threading.Thread(
            target=self._import_worker,
            args=(src, desired_output, stem, convert_script, bool(skip_enrichment)),
            name="tdvd-import-worker",
            daemon=True,
        )
        with self.import_lock:
            self.imp_thread = t
        t.start()
        return True, ""

    def _import_worker(
        self, src: str, desired_output: str, stem: str, convert_script: str, skip_enrichment: bool
    ) -> None:
        rip_tmp_dir = None
        conv_input = src
        final_archive = desired_output

        try:
            is_dev = src.startswith("/dev/")
            if not is_dev and os.path.exists(src):
                st = os.stat(src)
                is_dev = stat.S_ISBLK(st.st_mode) or stat.S_ISCHR(st.st_mode)

            if is_dev:
                dvdbackup_bin = shutil.which("dvdbackup")
                if not dvdbackup_bin:
                    raise RuntimeError(
                        "dvdbackup is required to rip directly from optical drive devices "
                        "(you can still import previously ripped VIDEO_TS folders, unencrypted .iso images, "
                        "or mounted unencrypted disc folders without it; to enable direct drive ripping, "
                        "install dvdbackup or run ./scripts/install-deps.sh --with-libdvdcss)."
                    )
                rip_tmp_dir = tempfile.mkdtemp(prefix="tdvd_rip_")
                with self.import_lock:
                    self.imp_state = IMPORT_RIPPING
                    self.imp_stage_msg = "Ripping DVD disc..."
                self._append_log(f">>> Running dvdbackup -M -i {src} -o {rip_tmp_dir}")
                self._notify_import_change()

                proc = subprocess.Popen(
                    [dvdbackup_bin, "-M", "-i", src, "-o", rip_tmp_dir],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    errors="replace",
                    start_new_session=True,
                )
                with self.import_lock:
                    self.imp_proc = proc
                if proc.stdout:
                    for line in proc.stdout:
                        self._append_log(line)
                        self._notify_import_change()
                rc = proc.wait()
                with self.import_lock:
                    self.imp_proc = None
                    cancelled = self.imp_cancel_requested
                if cancelled:
                    raise RuntimeError("Import cancelled.")
                if rc != 0:
                    raise RuntimeError(f"dvdbackup exited with status {rc}.")

                # Locate ripped directory
                subdirs = [
                    os.path.join(rip_tmp_dir, d)
                    for d in os.listdir(rip_tmp_dir)
                    if os.path.isdir(os.path.join(rip_tmp_dir, d))
                ]
                conv_input = subdirs[0] if subdirs else rip_tmp_dir

            with self.import_lock:
                self.imp_state = IMPORT_CONVERTING
                self.imp_stage_msg = "Converting to TinyDVD (.tdvd)..."
            self._append_log(f">>> Running convert-to-tinydvd {conv_input} -> {desired_output}")
            self._notify_import_change()

            cmd = [sys.executable, "-u", convert_script, conv_input, desired_output]
            if skip_enrichment:
                cmd.append("--skip-enrichment")

            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                start_new_session=True,
            )
            with self.import_lock:
                self.imp_proc = proc

            if proc.stdout:
                for line in proc.stdout:
                    s = line.rstrip("\r\n")
                    self._append_log(s)
                    if s.startswith("[output] Final archive: "):
                        final_archive = s[len("[output] Final archive: "):].strip()
                    elif s.startswith("[metadata]"):
                        with self.import_lock:
                            self.imp_stage_msg = "Enriching metadata..."
                    elif s.startswith("[package]"):
                        with self.import_lock:
                            self.imp_stage_msg = "Packaging .tdvd archive..."
                    elif s.startswith("=== "):
                        with self.import_lock:
                            self.imp_stage_msg = s.strip("= ").strip()[:200]
                    self._notify_import_change()

            rc = proc.wait()
            with self.import_lock:
                self.imp_proc = None
                cancelled = self.imp_cancel_requested
            if cancelled:
                raise RuntimeError("Import cancelled.")
            if rc != 0:
                raise RuntimeError(f"convert-to-tinydvd failed with exit status {rc}.")

            self.rescan()
            res_id = sanitize_id(os.path.basename(final_archive))
            matched = self.find_by_id(res_id)
            res_title = matched.title if matched else stem

            with self.import_lock:
                self.imp_state = IMPORT_DONE
                self.imp_active = False
                self.imp_stage_msg = "Import complete!"
                self.imp_result_disc_id = res_id
                self.imp_result_disc_path = final_archive
                self.imp_result_title = res_title
            self._notify_import_change()
        except Exception as exc:
            with self.import_lock:
                self.imp_proc = None
                self.imp_state = IMPORT_ERROR
                self.imp_active = False
                self.imp_stage_msg = "Import cancelled" if self.imp_cancel_requested else "Import failed"
                self.imp_error_msg = str(exc)[:511]
            self._notify_import_change()
        finally:
            if rip_tmp_dir and os.path.isdir(rip_tmp_dir):
                shutil.rmtree(rip_tmp_dir, ignore_errors=True)

    def cancel_import(self) -> None:
        proc = None
        with self.import_lock:
            self.imp_cancel_requested = True
            proc = self.imp_proc
        if proc and proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except OSError:
                try:
                    proc.terminate()
                except OSError:
                    pass
        self._notify_import_change()

    def destroy(self) -> None:
        self.cancel_import()
        self._watch_stop = True
        self._wake_watcher(b"q")
        with self.catalog_cond:
            self.catalog_cond.notify_all()
        if self._watch_thread and self._watch_thread.is_alive():
            self._watch_thread.join(timeout=2.0)
        with self.import_lock:
            t = self.imp_thread
        if t and t.is_alive():
            t.join(timeout=2.0)
        for fd_attr in ("_wake_r", "_wake_w"):
            fd = getattr(self, fd_attr, -1)
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
                setattr(self, fd_attr, -1)

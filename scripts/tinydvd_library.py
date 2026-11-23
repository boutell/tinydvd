#!/usr/bin/env python3
"""
Pure-Python library catalog, .tdvd archive reader, OS-native directory watcher
(kqueue on macOS, inotify on Linux), optical drive detector, and DVD import job manager.
"""

from __future__ import annotations

from dataclasses import dataclass
import ctypes
import ctypes.util
import hashlib
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
import traceback

import tinydvd_archive


IMPORT_IDLE = 0
IMPORT_RIPPING = 1
IMPORT_CONVERTING = 2
IMPORT_DONE = 3
IMPORT_ERROR = 4

IMPORT_MAX_LOGS = 64
SOURCE_DETECT_TTL_SEC = 5.0

IMPORT_STATE_NAMES = {
    IMPORT_IDLE: "idle",
    IMPORT_RIPPING: "ripping",
    IMPORT_CONVERTING: "converting",
    IMPORT_DONE: "done",
    IMPORT_ERROR: "error",
}


def sanitize_id(name: str) -> str:
    """Keep only characters that are safe in URLs and file names."""
    if not name:
        return ""
    return "".join(c if (c.isalnum() or c in "-_") else "_" for c in name)[:127]


def disc_id_for_path(path: str) -> str:
    """
    A stable, URL-safe ID for a disc. The readable part comes from the file
    name; the hash of the full name keeps "Alien: X" and "Alien_ X" apart.
    """
    base = os.path.basename(path.rstrip("/")) or path
    digest = hashlib.blake2b(base.encode("utf-8", errors="surrogateescape"), digest_size=4).hexdigest()
    return f"{sanitize_id(tinydvd_archive.strip_tdvd_extension(base))[:100]}-{digest}"


is_tdvd_file = tinydvd_archive.is_tdvd_path


def is_dvd_dir(dir_path: str) -> bool:
    if not dir_path:
        return False
    return (
        os.path.exists(os.path.join(dir_path, "VIDEO_TS.IFO"))
        or os.path.exists(os.path.join(dir_path, "VIDEO_TS", "VIDEO_TS.IFO"))
    )


def xdg_videos_dir(home: str, config_home: str | None = None) -> str | None:
    """
    The Videos folder the Linux desktop configured, from user-dirs.dirs
    (it is renamed or translated on some systems, e.g. ~/Vidéos), or None if
    it isn't set. A value equal to $HOME means "disabled" in the XDG spec.
    """
    config_home = config_home or os.path.join(home, ".config")
    try:
        with open(os.path.join(config_home, "user-dirs.dirs"), encoding="utf-8") as f:
            lines = f.read().splitlines()
    except (OSError, UnicodeDecodeError):
        return None
    for line in lines:
        m = re.match(r'\s*XDG_VIDEOS_DIR\s*=\s*"(.*)"\s*$', line)
        if not m:
            continue
        value = m.group(1).replace('\\"', '"')
        if value == "$HOME" or value.startswith("$HOME/"):
            value = home + value[len("$HOME"):]
        elif not value.startswith("/"):
            return None
        value = os.path.normpath(value)
        return None if value == os.path.normpath(home) else value
    return None


def default_library_path(home: str | None = None, platform: str | None = None,
                         config_home: str | None = None) -> str:
    """
    Where to look for movies when no path is given: ~/Movies on macOS; on
    Linux the desktop's configured Videos folder, then ~/Videos. Falls back
    to the other platform's convention if that folder exists instead.
    """
    home = home if home is not None else os.path.expanduser("~")
    if not home or home == "~":
        return "."
    platform = platform or sys.platform
    movies = os.path.join(home, "Movies")
    videos = os.path.join(home, "Videos")
    if platform == "darwin":
        candidates = [movies, videos]
    else:
        configured = xdg_videos_dir(home, config_home or os.environ.get("XDG_CONFIG_HOME") or None)
        candidates = [d for d in (configured, videos, movies) if d]
    for d in candidates:
        if os.path.isdir(d):
            return d
    return candidates[0]


# Imports work in hidden folders and files next to the library, with these
# prefixes: a disc ripped from a drive, the converter's staging folder, the
# archive being written, an extracted ISO
WORK_PREFIXES = (".tdvd_rip_", ".tdvd_stage_", ".tdvd_tmp_", ".tdvd_iso_")
# Leftovers nothing has written to for this long belong to an import that was
# killed (a crash, a power cut). Live imports write constantly
STALE_WORK_SECONDS = 3600
# The largest DVD (dual layer), assumed when a drive's disc size can't be read
DVD9_BYTES = 8_547_991_552
# Room to leave after an import, so it doesn't fill the disk to the last byte
SPACE_MARGIN_BYTES = 512 * 1024 * 1024


def _newest_mtime(path: str) -> float:
    newest = os.lstat(path).st_mtime
    for root, dirs, files in os.walk(path):
        for name in dirs + files:
            try:
                newest = max(newest, os.lstat(os.path.join(root, name)).st_mtime)
            except OSError:
                pass
    return newest


def sweep_stale_work(directory: str, now: float | None = None) -> int:
    """
    Delete import leftovers in directory that nothing has touched for
    STALE_WORK_SECONDS. Returns the bytes freed.
    """
    now = time.time() if now is None else now
    freed = 0
    try:
        entries = list(os.scandir(directory))
    except OSError:
        return 0
    for entry in entries:
        if not entry.name.startswith(WORK_PREFIXES):
            continue
        try:
            if now - _newest_mtime(entry.path) < STALE_WORK_SECONDS:
                continue
            size = _tree_size(entry.path)
            if entry.is_dir(follow_symlinks=False):
                shutil.rmtree(entry.path)
            else:
                os.unlink(entry.path)
            freed += size
        except OSError:
            pass
    return freed


def _tree_size(path: str) -> int:
    if not os.path.isdir(path) or os.path.islink(path):
        return os.lstat(path).st_size
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            try:
                total += os.lstat(os.path.join(root, name)).st_size
            except OSError:
                pass
    return total


def _disc_size(device: str) -> int:
    """Size of the disc in a drive, or DVD9_BYTES if it can't be read"""
    try:
        fd = os.open(device, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0))
        try:
            size = os.lseek(fd, 0, os.SEEK_END)
        finally:
            os.close(fd)
        if size > 0:
            return size
    except OSError:
        pass
    if sys.platform == "darwin":
        # Raw disk devices don't report a size by seeking; ask diskutil
        try:
            import plistlib
            out = subprocess.run(["diskutil", "info", "-plist", device.replace("/dev/rdisk", "/dev/disk")],
                                 capture_output=True, timeout=10).stdout
            size = int(plistlib.loads(out).get("TotalSize") or 0)
            if size > 0:
                return size
        except (OSError, ValueError, subprocess.SubprocessError, plistlib.InvalidFileException):
            pass
    return DVD9_BYTES


def import_space_needed(src: str, is_device: bool) -> int:
    """
    Roughly how many bytes an import needs free in the library folder.

    With D the source's size and T the finished archive's: the converter
    writes the converted files (T) to a staging folder, then copies them into
    the archive, briefly holding 2T. Archives are usually 20-35% of the DVD;
    allowing for up to half covers grainy films and many audio tracks.

    - A folder or ISO: peak 2T, at most D.
    - A drive (or, on Linux, an ISO, which is extracted first): the rip or
      extraction (D) plus the staged files (T) while converting. The rip is
      deleted before packaging, so the peak is D + T, at most 1.5 x D.
    """
    if is_device:
        return _disc_size(src) * 3 // 2
    size = _tree_size(src)
    if os.path.isfile(src) and sys.platform.startswith("linux"):
        return size * 3 // 2
    return size


def _nearly_full(directory: str) -> bool:
    try:
        return shutil.disk_usage(directory).free < 64 * 1024 * 1024
    except OSError:
        return False


def is_device_path(src: str) -> bool:
    if src.startswith("/dev/"):
        return True
    try:
        st = os.stat(src)
    except OSError:
        return False
    return stat.S_ISBLK(st.st_mode) or stat.S_ISCHR(st.st_mode)


def _unescape_mount_field(field: str) -> str:
    """/proc/self/mounts writes spaces and the like as octal escapes (\\040)"""
    return re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), field)


def optical_device_for(path: str, mounts_file: str = "/proc/self/mounts", platform: str | None = None) -> str:
    """
    The optical drive (e.g. /dev/sr0) whose disc is mounted at or above path,
    or "" if path isn't on a disc in a drive (Linux only).

    A commercial DVD's video files are usually CSS-encrypted, and the drive
    refuses to read those sectors (Input/output error) until something
    authenticates with it. Reading the mounted files can't do that; ripping
    from the drive with dvdbackup (libdvdcss) can, so imports rip mounted
    discs from their drive.
    """
    if not (platform or sys.platform).startswith("linux"):
        return ""
    try:
        target = os.path.realpath(path)
        with open(mounts_file, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return ""
    best_mount, best_device = "", ""
    for line in lines:
        parts = line.split()
        if len(parts) < 2:
            continue
        device, mount_point = _unescape_mount_field(parts[0]), _unescape_mount_field(parts[1])
        inside = target == mount_point or target.startswith(mount_point.rstrip("/") + "/")
        if inside and len(mount_point) > len(best_mount):
            best_mount, best_device = mount_point, device
    if not best_device.startswith("/dev/"):
        return ""
    real = os.path.realpath(best_device)
    return real if re.fullmatch(r"/dev/sr[0-9]+", real) else ""


def check_import_space(src: str, out_dir: str) -> str:
    """An explanation if out_dir lacks room to import src, else an empty string"""
    try:
        free = shutil.disk_usage(out_dir).free
    except OSError:
        return ""
    needed = import_space_needed(src, is_device_path(src)) + SPACE_MARGIN_BYTES
    if free >= needed:
        return ""
    return (f"Not enough disk space to import this disc into {out_dir}: it needs about "
            f"{format_size(needed)} free while importing, and {format_size(free)} is free. "
            "Free up some space or import into a folder on another disk.")


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


def _load_item(path: str) -> CatalogItem:
    base = os.path.basename(path.rstrip("/")) or path
    title = tinydvd_archive.strip_tdvd_extension(base)

    size_bytes = 0
    try:
        size_bytes = os.path.getsize(path)
    except OSError:
        pass

    thumb = tinydvd_archive.locate_member(path, tinydvd_archive.THUMBNAIL_NAME)
    meta = tinydvd_archive.read_metadata(path)

    year = 0
    try:
        year = max(0, int(meta.get("year") or 0))
    except (TypeError, ValueError):
        pass
    meta_title = str(meta.get("title") or "").strip()

    return CatalogItem(
        id=disc_id_for_path(path),
        title=(meta_title or title)[:255],
        path=path,
        size_bytes=size_bytes,
        size_str=format_size(size_bytes),
        has_thumbnail=bool(thumb and thumb.size > 0),
        year=year,
        director=_flatten_meta_field(meta.get("director"))[:127],
        cast=_flatten_meta_field(meta.get("cast"))[:1023],
        genres=_flatten_meta_field(meta.get("genres"))[:127],
        overview=_flatten_meta_field(meta.get("overview"))[:1023],
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
        self._detected_source: tuple[str, str] = ("", "")
        self._detected_at: float = 0.0

        self.rescan()

        # Reclaim space from imports a crash or power cut cut short
        if self.is_library:
            threading.Thread(target=sweep_stale_work, args=(self.target_path,),
                             name="tdvd-sweep", daemon=True).start()

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
                # A UI callback failing must not stop the watcher thread, but say so
                traceback.print_exc()

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
        return tinydvd_archive.read_thumbnail(disc_path) if disc_path else None

    def locate_video(self, disc_path: str, name: str) -> tinydvd_archive.MemberLocation | None:
        """Where a video or audio file's bytes are, inside an archive or a directory."""
        return tinydvd_archive.locate_member(disc_path, name)

    def _notify_import_change(self) -> None:
        if self.on_import_change:
            try:
                self.on_import_change()
            except Exception:
                traceback.print_exc()

    def _append_log(self, line: str) -> None:
        clean = (line or "").rstrip("\r\n")
        if not clean:
            return
        with self.import_lock:
            self.imp_logs.append(clean[:255])
            if len(self.imp_logs) > IMPORT_MAX_LOGS:
                self.imp_logs = self.imp_logs[-IMPORT_MAX_LOGS:]

    def _default_source(self) -> tuple[str, str]:
        """detect_default_source(), at most every few seconds: it walks /media."""
        now = time.monotonic()
        if now - self._detected_at > SOURCE_DETECT_TTL_SEC:
            self._detected_source = detect_default_source()
            self._detected_at = now
        return self._detected_source

    def get_import_status(self) -> dict:
        def_src, def_title = self._default_source()
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
        src = os.path.expanduser((source or "").strip())
        if not src:
            return False, "Missing DVD source path."
        if not os.path.exists(src):
            return False, f"No such file, folder or drive: {src}"

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

        # A disc mounted from a drive is ripped from the drive (see
        # optical_device_for); its folder name still makes the title
        rip_src = optical_device_for(src) or src

        with self.import_lock:
            busy = self.imp_active
        if not busy:
            sweep_stale_work(out_dir)
            space_problem = check_import_space(rip_src, out_dir)
            if space_problem:
                return False, space_problem

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
            args=(rip_src, desired_output, stem, convert_script, bool(skip_enrichment)),
            name="tdvd-import-worker",
            daemon=True,
        )
        with self.import_lock:
            self.imp_thread = t
        t.start()
        return True, ""

    def _run_import_step(self, cmd: list[str], on_line) -> int:
        """
        Run one import step, streaming its output to on_line. Registers the
        process under the lock, so a cancel can't slip in between starting it
        and being able to kill it. Returns its exit status.
        """
        with self.import_lock:
            if self.imp_cancel_requested:
                raise RuntimeError("Import cancelled.")
            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                start_new_session=True,
            )
            self.imp_proc = proc
        try:
            if proc.stdout:
                for line in proc.stdout:
                    on_line(line.rstrip("\r\n"))
            rc = proc.wait()
        finally:
            with self.import_lock:
                self.imp_proc = None
                cancelled = self.imp_cancel_requested
        if cancelled:
            raise RuntimeError("Import cancelled.")
        return rc

    def _import_worker(
        self, src: str, desired_output: str, stem: str, convert_script: str, skip_enrichment: bool
    ) -> None:
        rip_tmp_dir = None
        conv_input = src
        final_archive = desired_output

        try:
            is_dev = is_device_path(src)

            if is_dev:
                dvdbackup_bin = shutil.which("dvdbackup")
                if not dvdbackup_bin:
                    raise RuntimeError(
                        f"Importing a disc from a drive ({src}) needs dvdbackup, and libdvdcss for "
                        "encrypted (most commercial) DVDs. Reinstall with './install --with-libdvdcss' "
                        "to add them. Previously ripped VIDEO_TS folders and unencrypted .iso images "
                        "import without them."
                    )
                # Rip next to the library, not into the system temp folder,
                # which can be small (on many Linux systems it's in RAM)
                rip_tmp_dir = tempfile.mkdtemp(prefix=".tdvd_rip_", dir=os.path.dirname(desired_output) or ".")
                with self.import_lock:
                    self.imp_state = IMPORT_RIPPING
                    self.imp_stage_msg = "Ripping DVD disc..."
                self._append_log(f">>> Running dvdbackup -M -i {src} -o {rip_tmp_dir}")
                self._notify_import_change()

                def on_rip_line(line: str) -> None:
                    self._append_log(line)
                    self._notify_import_change()

                rc = self._run_import_step([dvdbackup_bin, "-M", "-i", src, "-o", rip_tmp_dir], on_rip_line)
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
            if rip_tmp_dir:
                cmd.append("--delete-rip-before-packaging")
            if skip_enrichment:
                cmd.append("--skip-enrichment")

            def on_convert_line(s: str) -> None:
                nonlocal final_archive
                self._append_log(s)
                if s.startswith("[output] Final archive: "):
                    final_archive = s[len("[output] Final archive: "):].strip()
                elif s.startswith("[metadata]"):
                    with self.import_lock:
                        self.imp_stage_msg = "Looking up movie details..."
                elif s.startswith("[package]"):
                    with self.import_lock:
                        self.imp_stage_msg = "Packaging .tdvd archive..."
                elif s.startswith("=== "):
                    with self.import_lock:
                        self.imp_stage_msg = s.strip("= ").strip()[:200]
                self._notify_import_change()

            rc = self._run_import_step(cmd, on_convert_line)
            if rc != 0:
                raise RuntimeError(f"convert-to-tinydvd failed with exit status {rc}.")

            self.rescan()
            res_id = disc_id_for_path(final_archive)
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
                msg = str(exc)
                if not self.imp_cancel_requested and _nearly_full(os.path.dirname(desired_output) or "."):
                    msg += " The disk is full, which is the likely cause: free up some space and try again."
                self.imp_error_msg = msg[:511]
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

"""
Shared helpers for TinyDVD packages.

A package is either a .tdvd archive (an uncompressed ZIP, read in place) or an
unpacked TinyDVD directory. Every Python tool reads and writes packages through
this module, so they agree on file names, on size limits, and on how an
archive is changed without risking the original.
"""

from __future__ import annotations

import json
import os
import shutil
import struct
import tempfile
import time
import zipfile
from typing import BinaryIO, NamedTuple

TDVD_EXTENSIONS = (".tdvd", ".tinydvd")
THUMBNAIL_NAME = "thumbnail.png"
METADATA_NAME = "metadata.json"

MAX_THUMBNAIL_BYTES = 16 * 1024 * 1024
MAX_METADATA_BYTES = 1024 * 1024

COPY_CHUNK = 1024 * 1024
ZIP_LOCAL_HEADER_LEN = 30


class VolumeInfo(NamedTuple):
    label: str      # e.g. "DUNE"; "" if unset
    mastered: str   # when the disc was mastered, "YYYY-MM-DD"; "" if unset or implausible


def read_volume_info(path: str) -> VolumeInfo:
    """
    The volume label and mastering date of a disc in a drive (e.g. /dev/sr0)
    or a disc image (.iso), from its ISO 9660 primary volume descriptor, which
    DVD-Videos carry alongside UDF. Never encrypted, so no libdvdcss needed.
    """
    try:
        with open(path, "rb") as f:
            f.seek(16 * 2048)
            pvd = f.read(2048)
    except OSError:
        return VolumeInfo("", "")
    if len(pvd) < 830 or pvd[0] != 1 or pvd[1:6] != b"CD001":
        return VolumeInfo("", "")
    label = pvd[40:72].decode("ascii", "replace").strip(" \x00")
    # Volume creation date: "YYYYMMDDHHMMSScc" plus a time zone byte; all
    # zeros or junk when the authoring tool didn't fill it in. DVD-Video
    # began in 1996, so anything earlier (or in the future) is junk too
    stamp = pvd[813:821].decode("ascii", "replace")
    mastered = ""
    if stamp.isdigit():
        y, m, d = int(stamp[:4]), int(stamp[4:6]), int(stamp[6:8])
        if 1996 <= y <= time.localtime().tm_year + 1 and 1 <= m <= 12 and 1 <= d <= 31:
            mastered = f"{y:04d}-{m:02d}-{d:02d}"
    return VolumeInfo(label, mastered)


def is_tdvd_path(path: str) -> bool:
    """True if path names a .tdvd / .tinydvd archive (by extension, any case)."""
    return bool(path) and path.lower().endswith(TDVD_EXTENSIONS)


def strip_tdvd_extension(name: str) -> str:
    for ext in TDVD_EXTENSIONS:
        if name.lower().endswith(ext):
            return name[: -len(ext)]
    return name


def is_archive(path: str) -> bool:
    return is_tdvd_path(path) and os.path.isfile(path) and zipfile.is_zipfile(path)


def find_member(zf: zipfile.ZipFile, name: str) -> zipfile.ZipInfo | None:
    """The archive entry whose base name matches name, ignoring case."""
    target = name.lower()
    for info in zf.infolist():
        if os.path.basename(info.filename).lower() == target:
            return info
    return None


def _find_dir_file(dir_path: str, name: str) -> str | None:
    exact = os.path.join(dir_path, name)
    if os.path.isfile(exact):
        return exact
    target = name.lower()
    try:
        for entry in os.listdir(dir_path):
            if entry.lower() == target:
                full = os.path.join(dir_path, entry)
                if os.path.isfile(full):
                    return full
    except OSError:
        pass
    return None


def stored_data_offset(raw: BinaryIO, info: zipfile.ZipInfo) -> int:
    """Byte offset of an entry's data, read from its local file header."""
    raw.seek(info.header_offset)
    header = raw.read(ZIP_LOCAL_HEADER_LEN)
    if len(header) < ZIP_LOCAL_HEADER_LEN or header[:4] != b"PK\x03\x04":
        raise ValueError(f"bad local header for {info.filename}")
    name_len, extra_len = struct.unpack("<HH", header[26:30])
    return info.header_offset + ZIP_LOCAL_HEADER_LEN + name_len + extra_len


class MemberLocation(NamedTuple):
    """Where a package member's bytes live.

    kind is "file" (a plain file: path), "slice" (stored archive entry: the
    bytes path[offset:offset+size]) or "zip" (compressed archive entry named
    member, which has to be decompressed to read).
    """

    kind: str
    path: str
    offset: int
    size: int
    member: str = ""

    def ffmpeg_input(self) -> str | None:
        """A URL ffmpeg can open and seek in, or None if it must be piped."""
        if self.kind == "file":
            return "file:" + os.path.abspath(self.path)
        if self.kind == "slice":
            end = self.offset + self.size
            return f"subfile,,start,{self.offset},end,{end},,:file:{os.path.abspath(self.path)}"
        return None

    def copy_to(self, out: BinaryIO) -> None:
        """Write the member's bytes to out. Lets OSError (e.g. a closed pipe) through."""
        if self.kind == "zip":
            with zipfile.ZipFile(self.path, "r") as zf, zf.open(self.member) as stream:
                shutil.copyfileobj(stream, out, COPY_CHUNK)
            return
        with open(self.path, "rb") as f:
            f.seek(self.offset)
            remaining = self.size
            while remaining > 0:
                chunk = f.read(min(COPY_CHUNK, remaining))
                if not chunk:
                    break
                out.write(chunk)
                remaining -= len(chunk)


def locate_member(package: str, name: str) -> MemberLocation | None:
    """Find a member (by base name, ignoring case) in an archive or directory."""
    if not package or not name:
        return None
    if os.path.isdir(package):
        full = _find_dir_file(package, name)
        if not full:
            return None
        return MemberLocation("file", full, 0, os.path.getsize(full))
    if not is_archive(package):
        return None
    try:
        with zipfile.ZipFile(package, "r") as zf:
            info = find_member(zf, name)
            if not info:
                return None
            if info.compress_type == zipfile.ZIP_STORED:
                with open(package, "rb") as raw:
                    offset = stored_data_offset(raw, info)
                return MemberLocation("slice", package, offset, info.file_size)
            return MemberLocation("zip", package, 0, info.file_size, info.filename)
    except (OSError, ValueError, zipfile.BadZipFile):
        return None


def list_members(package: str) -> dict[str, int]:
    """{base name: size} of the files in a package."""
    out: dict[str, int] = {}
    try:
        if os.path.isdir(package):
            for entry in os.scandir(package):
                if entry.is_file():
                    out[entry.name] = entry.stat().st_size
        elif is_archive(package):
            with zipfile.ZipFile(package, "r") as zf:
                for info in zf.infolist():
                    base = os.path.basename(info.filename)
                    if base:
                        out[base] = info.file_size
    except (OSError, zipfile.BadZipFile):
        pass
    return out


def read_member(package: str, name: str, max_bytes: int) -> bytes | None:
    """A member's bytes, or None if it is missing, empty or over max_bytes."""
    loc = locate_member(package, name)
    if not loc or loc.size <= 0 or loc.size > max_bytes:
        return None
    try:
        if loc.kind == "zip":
            with zipfile.ZipFile(loc.path, "r") as zf, zf.open(loc.member) as stream:
                data = stream.read(max_bytes + 1)
        else:
            with open(loc.path, "rb") as f:
                f.seek(loc.offset)
                data = f.read(loc.size)
    except (OSError, zipfile.BadZipFile):
        return None
    return data if 0 < len(data) <= max_bytes else None


def read_thumbnail(package: str) -> bytes | None:
    return read_member(package, THUMBNAIL_NAME, MAX_THUMBNAIL_BYTES)


def _load_json(data: bytes | None) -> dict:
    if not data:
        return {}
    try:
        obj = json.loads(data.decode("utf-8", errors="replace"))
    except ValueError:
        return {}
    return obj if isinstance(obj, dict) else {}


def sidecar_metadata_path(package: str) -> str:
    """The <name>.json written next to an archive."""
    return os.path.splitext(package)[0] + ".json"


def read_metadata(package: str) -> dict:
    """The package's metadata.json, or for an archive without one, its sidecar .json."""
    meta = _load_json(read_member(package, METADATA_NAME, MAX_METADATA_BYTES))
    if meta or os.path.isdir(package):
        return meta
    sidecar = sidecar_metadata_path(package)
    try:
        if 0 < os.path.getsize(sidecar) <= MAX_METADATA_BYTES:
            with open(sidecar, "rb") as f:
                return _load_json(f.read())
    except OSError:
        pass
    return {}


# Temporary files are created readable only by their owner; finished files
# get the usual permissions, or keep those of the file they replace
NEW_FILE_MODE = 0o644


def _copy_mode(tmp: str, original: str) -> None:
    try:
        mode = os.stat(original).st_mode & 0o7777
    except OSError:
        mode = NEW_FILE_MODE
    os.chmod(tmp, mode)


def write_file_atomically(path: str, data: bytes) -> None:
    """Write a file so readers see the old or the new contents, never a mix."""
    fd, tmp = tempfile.mkstemp(prefix="." + os.path.basename(path) + ".", suffix=".tmp",
                               dir=os.path.dirname(os.path.abspath(path)))
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        _copy_mode(tmp, path)
        os.replace(tmp, path)
    except BaseException:
        _unlink_quietly(tmp)
        raise


def _unlink_quietly(path: str) -> None:
    try:
        os.unlink(path)
    except OSError:
        pass


def _new_member_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=time.localtime()[:6])
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    return info


def rewrite_archive(archive_path: str, replacements: dict[str, bytes]) -> None:
    """
    Add or replace members of an archive (matched by base name, ignoring case).

    The archive is copied to a hidden temporary file next to it and renamed
    over the original, so a crash or full disk leaves the original intact.
    This costs a full copy of the archive, which is the price of never
    rewriting a multi-GB file in place.
    """
    replace_lower = {name.lower() for name in replacements}
    parent = os.path.dirname(os.path.abspath(archive_path))
    fd, tmp = tempfile.mkstemp(prefix="." + os.path.basename(archive_path) + ".", suffix=".tmp", dir=parent)
    os.close(fd)
    try:
        with zipfile.ZipFile(archive_path, "r") as zin, \
                zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as zout:
            for info in zin.infolist():
                if os.path.basename(info.filename).lower() in replace_lower:
                    continue
                copy = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                copy.compress_type = info.compress_type
                copy.external_attr = info.external_attr
                copy.file_size = info.file_size  # lets zipfile pick ZIP64 when needed
                with zin.open(info) as src, zout.open(copy, "w") as dst:
                    shutil.copyfileobj(src, dst, COPY_CHUNK)
            for name, data in replacements.items():
                zout.writestr(_new_member_info(name), data)
        with open(tmp, "rb+") as f:
            os.fsync(f.fileno())
        _copy_mode(tmp, archive_path)
        os.replace(tmp, archive_path)
    except BaseException:
        _unlink_quietly(tmp)
        raise


def write_members(package: str, members: dict[str, bytes]) -> None:
    """Add or replace files in a package directory or archive, safely."""
    if os.path.isdir(package):
        for name, data in members.items():
            write_file_atomically(os.path.join(package, name), data)
    elif is_archive(package):
        rewrite_archive(package, members)
    else:
        raise ValueError(f"{package} is not a TinyDVD package")


def create_archive(output_path: str, files: list[tuple[str, str]], extra: dict[str, bytes] | None = None) -> None:
    """Write a new ZIP_STORED archive of (archive name, source path) files plus extra members."""
    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as zf:
        for arcname, src in files:
            zf.write(src, arcname=arcname)
        for name, data in (extra or {}).items():
            zf.writestr(_new_member_info(name), data)


def place_without_overwrite(tmp_path: str, dest_path: str) -> bool:
    """
    Move tmp_path to dest_path unless dest_path already exists. Returns False
    if it does. The check and the move are one atomic step, so two imports
    racing for the same name can't overwrite each other.
    """
    os.chmod(tmp_path, NEW_FILE_MODE)
    try:
        os.link(tmp_path, dest_path)
    except FileExistsError:
        return False
    except OSError:
        # No hard links on this filesystem: reserve the name with an
        # exclusive create, then rename over the placeholder
        try:
            fd = os.open(dest_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except FileExistsError:
            return False
        os.close(fd)
        os.replace(tmp_path, dest_path)
        return True
    _unlink_quietly(tmp_path)
    return True

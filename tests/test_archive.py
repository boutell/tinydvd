#!/usr/bin/env python3
"""
Tests for scripts/tinydvd_archive.py, the package helpers every Python tool
shares, and for disc IDs.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import zipfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))

import tinydvd_archive  # noqa: E402
from tinydvd_library import default_library_path, disc_id_for_path, xdg_videos_dir  # noqa: E402


def make_archive(path: str, members: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as zf:
        for name, data in members.items():
            zf.writestr(name, data)


def test_locate_and_read(tmp: str) -> None:
    arc = os.path.join(tmp, "Movie.TDVD")
    make_archive(arc, {"VIDEO_TS.IFO": b"ifo", "Thumbnail.PNG": b"\x89PNGdata", "VTS_01_VIDEO.mkv": b"mkvbytes"})
    assert tinydvd_archive.is_tdvd_path(arc)
    loc = tinydvd_archive.locate_member(arc, "vts_01_video.MKV")
    assert loc and loc.kind == "slice" and loc.size == 8
    with open(arc, "rb") as f:
        f.seek(loc.offset)
        assert f.read(loc.size) == b"mkvbytes"
    assert loc.ffmpeg_input().startswith("subfile,,start,")
    assert tinydvd_archive.read_thumbnail(arc) == b"\x89PNGdata"
    assert tinydvd_archive.read_member(arc, "VIDEO_TS.IFO", max_bytes=2) is None  # over the limit
    assert tinydvd_archive.locate_member(arc, "missing.mkv") is None
    print("  - Case-insensitive lookup, in-place byte ranges, size limits: OK")


def test_rewrite_keeps_original_on_failure(tmp: str) -> None:
    arc = os.path.join(tmp, "Keep.tdvd")
    make_archive(arc, {"a.mkv": b"A" * 1000, "Metadata.JSON": b'{"title": "Old"}'})
    tinydvd_archive.write_members(arc, {"metadata.json": b'{"title": "New"}', "thumbnail.png": b"png"})
    with zipfile.ZipFile(arc) as zf:
        names = zf.namelist()
        assert sorted(names) == ["a.mkv", "metadata.json", "thumbnail.png"], names
        assert zf.read("a.mkv") == b"A" * 1000
    assert tinydvd_archive.read_metadata(arc)["title"] == "New"

    # A failure part way through leaves the original untouched and no temp files
    before = open(arc, "rb").read()

    class Boom(Exception):
        pass

    real_copy = shutil.copyfileobj

    def failing_copy(*_args, **_kwargs):
        raise Boom()

    shutil.copyfileobj = failing_copy
    try:
        tinydvd_archive.rewrite_archive(arc, {"metadata.json": b"{}"})
        raise AssertionError("expected failure")
    except Boom:
        pass
    finally:
        shutil.copyfileobj = real_copy
    assert open(arc, "rb").read() == before
    assert [n for n in os.listdir(tmp) if n.endswith(".tmp")] == []
    print("  - Members replaced by atomic rewrite; original intact after a failure: OK")


def test_sidecar_metadata(tmp: str) -> None:
    arc = os.path.join(tmp, "Side.tdvd")
    make_archive(arc, {"a.mkv": b"x"})
    with open(os.path.join(tmp, "Side.json"), "w") as f:
        json.dump({"title": "From Sidecar"}, f)
    assert tinydvd_archive.read_metadata(arc)["title"] == "From Sidecar"
    print("  - Sidecar metadata fallback: OK")


def test_place_without_overwrite(tmp: str) -> None:
    dest = os.path.join(tmp, "Taken.tdvd")
    with open(dest, "wb") as f:
        f.write(b"original")
    src = os.path.join(tmp, ".new.tmp")
    with open(src, "wb") as f:
        f.write(b"new")
    assert not tinydvd_archive.place_without_overwrite(src, dest)
    assert open(dest, "rb").read() == b"original"
    free = os.path.join(tmp, "Free.tdvd")
    assert tinydvd_archive.place_without_overwrite(src, free)
    assert open(free, "rb").read() == b"new" and not os.path.exists(src)
    print("  - Never overwrites an existing archive: OK")


def test_disc_ids() -> None:
    a = disc_id_for_path("/lib/Alien: X.tdvd")
    b = disc_id_for_path("/lib/Alien_ X.tdvd")
    assert a != b, (a, b)
    assert a == disc_id_for_path("/elsewhere/Alien: X.tdvd")
    assert all(c.isalnum() or c in "-_" for c in a)
    print("  - Distinct, URL-safe disc IDs for similar names: OK")


def test_default_library_path(tmp: str) -> None:
    home = os.path.join(tmp, "home")
    cfg = os.path.join(home, ".config")
    os.makedirs(cfg)
    # Nothing exists: each platform's convention
    assert default_library_path(home, "darwin") == os.path.join(home, "Movies")
    assert default_library_path(home, "linux") == os.path.join(home, "Videos")
    # A translated Videos folder from user-dirs.dirs wins on Linux
    with open(os.path.join(cfg, "user-dirs.dirs"), "w", encoding="utf-8") as f:
        f.write('# comment\nXDG_DESKTOP_DIR="$HOME/Bureau"\nXDG_VIDEOS_DIR="$HOME/Vid\u00e9os"\n')
    vids = os.path.join(home, "Vid\u00e9os")
    assert xdg_videos_dir(home) == vids
    assert default_library_path(home, "linux") == vids  # configured, even before it exists
    os.makedirs(os.path.join(home, "Videos"))
    assert default_library_path(home, "linux") == os.path.join(home, "Videos")  # configured one missing
    os.makedirs(vids)
    assert default_library_path(home, "linux") == vids
    # macOS ignores the XDG file; Movies falls back to Videos
    assert default_library_path(home, "darwin") == os.path.join(home, "Videos")
    # Absolute paths, "disabled" ($HOME itself) and a custom XDG_CONFIG_HOME
    other = os.path.join(tmp, "other")
    os.makedirs(other)
    with open(os.path.join(other, "user-dirs.dirs"), "w", encoding="utf-8") as f:
        f.write('XDG_VIDEOS_DIR="/srv/media"\n')
    assert xdg_videos_dir(home, other) == "/srv/media"
    with open(os.path.join(other, "user-dirs.dirs"), "w", encoding="utf-8") as f:
        f.write('XDG_VIDEOS_DIR="$HOME/"\n')
    assert xdg_videos_dir(home, other) is None
    print("  - Default library folder, including translated XDG Videos: OK")


def main() -> int:
    print("=== Running TinyDVD archive helper tests ===")
    tmp = tempfile.mkdtemp(prefix="tinydvd_test_archive_")
    try:
        test_locate_and_read(tmp)
        test_rewrite_keeps_original_on_failure(tmp)
        test_sidecar_metadata(tmp)
        test_place_without_overwrite(tmp)
        test_disc_ids()
        test_default_library_path(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("All archive helper tests passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())

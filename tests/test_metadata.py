#!/usr/bin/env python3
"""
Tests for picking the right film among several with the same title
(scripts/tinydvd_metadata.py), and for reading a disc's label and mastering
date. Offline: Wikidata's answers are canned.
"""

from __future__ import annotations

import os
import struct
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))

import tinydvd_archive  # noqa: E402
import tinydvd_library  # noqa: E402
import tinydvd_metadata as M  # noqa: E402

MINUTE = "http://www.wikidata.org/entity/Q7727"


def film(qid: str, year: int, minutes: list[int], sitelinks: int, label: str = "Dune",
         cancelled_year: int = 0) -> dict:
    dates = [{"mainsnak": {"datavalue": {"value": {"time": f"+{year}-10-22T00:00:00Z"}}}}]
    if cancelled_year:  # a release date announced, then cancelled
        dates.append({"rank": "deprecated",
                      "mainsnak": {"datavalue": {"value": {"time": f"+{cancelled_year}-10-20T00:00:00Z"}}}})
    return {
        "id": qid,
        "labels": {"en": {"value": label}},
        "claims": {
            "P577": dates,
            "P2047": [{"mainsnak": {"datavalue": {"value": {"amount": f"+{m}", "unit": MINUTE}}}}
                      for m in minutes],
        },
        "sitelinks": {f"wiki{i}": {} for i in range(sitelinks)},
    }


# Two films called Dune: Lynch's (1984; 137 min, and a 190 min TV cut) and
# Villeneuve's (2021; 155 min), the better known one; and its sequel, whose
# first release date was cancelled
FILMS = {"Q1": film("Q1", 1984, [137, 190], 80), "Q2": film("Q2", 2021, [155], 90),
         "Q3": film("Q3", 2024, [166], 70, label="Dune: Part Two", cancelled_year=2023)}


def fake_get_json(url: str, params: dict | None = None) -> dict:
    params = params or {}
    if params.get("list") == "search":
        return {"query": {"search": [{"title": q} for q in FILMS]}}
    if params.get("action") == "wbgetentities":
        return {"entities": {q: FILMS[q] for q in params["ids"].split("|") if q in FILMS}}
    return {}


def pick(title: str = "Dune", **kw) -> int:
    meta = M.fetch_wikidata_metadata(title, **kw)
    assert meta, kw
    return meta["year"]


def test_choosing_among_remakes() -> None:
    real = M._get_json
    M._get_json = fake_get_json
    try:
        assert pick() == 2021                                  # nothing to go on: the better known
        assert pick(runtime_min=155) == 2021                   # the running time decides
        assert pick(runtime_min=137) == 1984
        assert pick(runtime_min=189) == 1984                   # any of a film's cuts counts
        assert pick(max_year=2000) == 1984                     # a disc from 2000 can't hold the 2021 film
        assert pick(max_year=2022) == 2021
        assert pick(runtime_min=155, max_year=2003) == 1984    # the disc's date beats a running time
        assert pick(year=1984, max_year=2022) == 1984          # a year given outright beats everything
        assert pick(max_year=1990) == 1984
        # Punctuation doesn't matter, and a cancelled release date doesn't count
        assert pick("Dune Part Two") == 2024
        # Junk at the end of a disc label is dropped, if the film fits the disc
        assert pick("Dune Part Two Ok", runtime_min=166, max_year=2024) == 2024
        assert pick("Dune Ok", runtime_min=137) == 1984
        assert M.fetch_wikidata_metadata("Dune Part Two Ok") is None             # nothing to check it with
        assert M.fetch_wikidata_metadata("Dune Part Two Ok", runtime_min=100) is None  # doesn't fit
        assert M.fetch_wikidata_metadata("Dune Part Two Ok", runtime_min=166, max_year=2022) is None
    finally:
        M._get_json = real
    print("  - Choosing among films with the same title (year, disc date, running time, disc label junk): OK")


def test_volume_info(tmp: str) -> None:
    def image(label: bytes, date: bytes) -> str:
        pvd = bytearray(2048)
        pvd[0], pvd[1:6] = 1, b"CD001"
        pvd[40:72] = label.ljust(32)
        pvd[813:830] = date.ljust(17, b"0")
        path = os.path.join(tmp, "disc.iso")
        with open(path, "wb") as f:
            f.write(b"\0" * 16 * 2048 + bytes(pvd))
        return path

    info = tinydvd_archive.read_volume_info(image(b"DUNE", b"2022011812000000"))
    assert info == ("DUNE", "2022-01-18"), info
    # Unset (all zeros or spaces) and impossible dates are ignored
    for junk in (b"0000000000000000", b" " * 16, b"1970010100000000", b"2022139900000000", b"9999123100000000"):
        assert tinydvd_archive.read_volume_info(image(b"DUNE", junk)) == ("DUNE", ""), junk
    assert tinydvd_archive.read_volume_info(tmp) == ("", "")  # a folder
    print("  - Disc label and mastering date from the volume descriptor: OK")


def make_iso(label: str, date: str, files: dict[str, bytes]) -> bytes:
    """A minimal ISO 9660 image with these files in VIDEO_TS"""
    def record(name: bytes, lba: int, size: int, is_dir: bool) -> bytes:
        rec = bytearray(33 + len(name) + (1 - len(name) % 2))
        rec[0] = len(rec)
        rec[2:10] = struct.pack("<I", lba) + struct.pack(">I", lba)
        rec[10:18] = struct.pack("<I", size) + struct.pack(">I", size)
        rec[25] = 2 if is_dir else 0
        rec[32] = len(name)
        rec[33:33 + len(name)] = name
        return bytes(rec)

    def sector(data: bytes) -> bytes:
        return data + b"\0" * (-len(data) % 2048)

    names = sorted(files)
    lbas, lba = {}, 20   # 16 PVD, 17 terminator, 18 root, 19 VIDEO_TS
    for n in names:
        lbas[n] = lba
        lba += (len(files[n]) + 2047) // 2048
    root = record(b"\0", 18, 2048, True) + record(b"\1", 18, 2048, True) + record(b"VIDEO_TS", 19, 2048, True)
    video_ts = record(b"\0", 19, 2048, True) + record(b"\1", 18, 2048, True) + b"".join(
        record(f"{n.upper()};1".encode(), lbas[n], len(files[n]), False) for n in names)
    pvd = bytearray(2048)
    pvd[0], pvd[1:6] = 1, b"CD001"
    pvd[40:72] = label.encode().ljust(32)
    pvd[156:190] = record(b"\0", 18, 2048, True)
    pvd[813:830] = date.encode().ljust(17, b"0")
    terminator = bytes([255]) + b"CD001" + b"\0" * 2042
    return (b"\0" * 16 * 2048 + bytes(pvd) + terminator + sector(root) + sector(video_ts)
            + b"".join(sector(files[n]) for n in names))


def test_import_preview(tmp: str) -> None:
    """tinydvd --lookup names and looks up a disc as an import would"""
    mounts = os.path.join(tmp, "mounts")
    with open(mounts, "w") as f:
        f.write("/dev/sda1 / ext4 rw 0 0\n/dev/sr0 /media/me/DUNE\\040PART rw 0 0\n")
    assert tinydvd_library.mount_point_of("/dev/sr0", mounts) == "/media/me/DUNE PART"
    assert tinydvd_library.mount_point_of("/dev/sr1", mounts) == ""

    assert tinydvd_library.import_output_stem("/x/DUNE_PART_TWO_OK") == "Dune Part Two Ok"
    assert tinydvd_library.import_output_stem("/x/movie.iso") == "movie"
    assert tinydvd_library.import_output_stem("/x/DUNE", "Dune: Part Two") == "Dune Part Two"

    # A disc's folder, as mounted: VIDEO_TS holds the IFOs
    disc = os.path.join(tmp, "DUNE_PART_TWO_OK")
    os.makedirs(os.path.join(disc, "VIDEO_TS"))
    fixture = os.path.join(REPO_ROOT, "tests", "fixtures", "TestDisc.tdvd")
    for name in tinydvd_archive.list_members(fixture):
        if name.upper().endswith(".IFO"):
            with open(os.path.join(disc, "VIDEO_TS", name), "wb") as f:
                f.write(tinydvd_archive.read_member(fixture, name, 1 << 24))
    searched = []

    def fake(url: str, params: dict | None = None) -> dict:
        if (params or {}).get("list") == "search":
            searched.append(params["srsearch"])
        return fake_get_json(url, params)

    real = M._get_json
    M._get_json = fake
    try:
        meta = tinydvd_library.preview_import_metadata(disc)
    finally:
        M._get_json = real
    # Looked up by the name the import would give it; the test disc's
    # running time fits no Dune, so nothing is found
    assert searched and searched[0].startswith("Dune Part Two Ok "), searched
    assert meta["title"] == "Dune Part Two Ok" and meta["disc_id"], meta
    # The same disc as an image (as a drive would read): its IFOs are read
    # straight from the ISO 9660 file system, and the disc's date is used
    ifos = {n: tinydvd_archive.read_member(fixture, n, 1 << 24)
            for n in tinydvd_archive.list_members(fixture) if n.upper().endswith(".IFO")}
    iso = os.path.join(tmp, "DUNE_PART_TWO_OK.iso")
    with open(iso, "wb") as f:
        f.write(make_iso("DUNE_PART_TWO_OK", "2024060112000000", ifos))
    assert tinydvd_archive.read_disc_ifos(iso) == {n.upper(): d for n, d in ifos.items()}
    M._get_json = fake
    try:
        from_image = tinydvd_library.preview_import_metadata(iso)
    finally:
        M._get_json = real
    assert from_image["disc_id"] == meta["disc_id"] and from_image["disc_mastered"] == "2024-06-01", from_image
    assert tinydvd_archive.read_disc_ifos(os.path.join(tmp, "mounts")) == {}  # not a disc

    try:
        tinydvd_library.preview_import_metadata(tmp)
        raise AssertionError("not a disc")
    except ValueError:
        pass
    print("  - Previewing an import's movie details (tinydvd --lookup): OK")


def main() -> int:
    print("=== Movie details lookup tests ===")
    with tempfile.TemporaryDirectory() as tmp:
        test_choosing_among_remakes()
        test_volume_info(tmp)
        test_import_preview(tmp)
    print("All movie details lookup tests passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())

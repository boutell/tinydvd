#!/usr/bin/env python3
"""
Tests for picking the right film among several with the same title
(scripts/tinydvd_metadata.py), and for reading a disc's label and mastering
date. Offline: Wikidata's answers are canned.
"""

from __future__ import annotations

import os
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))

import tinydvd_archive  # noqa: E402
import tinydvd_metadata as M  # noqa: E402

MINUTE = "http://www.wikidata.org/entity/Q7727"


def film(qid: str, year: int, minutes: list[int], sitelinks: int) -> dict:
    return {
        "id": qid,
        "labels": {"en": {"value": "Dune"}},
        "claims": {
            "P577": [{"mainsnak": {"datavalue": {"value": {"time": f"+{year}-10-22T00:00:00Z"}}}}],
            "P2047": [{"mainsnak": {"datavalue": {"value": {"amount": f"+{m}", "unit": MINUTE}}}}
                      for m in minutes],
        },
        "sitelinks": {f"wiki{i}": {} for i in range(sitelinks)},
    }


# Two films called Dune: Lynch's (1984; 137 min, and a 190 min TV cut) and
# Villeneuve's (2021; 155 min), the better known one
FILMS = {"Q1": film("Q1", 1984, [137, 190], 80), "Q2": film("Q2", 2021, [155], 90)}


def fake_get_json(url: str, params: dict | None = None) -> dict:
    params = params or {}
    if params.get("list") == "search":
        return {"query": {"search": [{"title": q} for q in FILMS]}}
    if params.get("action") == "wbgetentities":
        return {"entities": {q: FILMS[q] for q in params["ids"].split("|") if q in FILMS}}
    return {}


def pick(**kw) -> int:
    meta = M.fetch_wikidata_metadata("Dune", **kw)
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
    finally:
        M._get_json = real
    print("  - Choosing among films with the same title (year, disc date, running time): OK")


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


def main() -> int:
    print("=== Movie details lookup tests ===")
    with tempfile.TemporaryDirectory() as tmp:
        test_choosing_among_remakes()
        test_volume_info(tmp)
    print("All movie details lookup tests passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())

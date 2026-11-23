#!/usr/bin/env python3
"""
TinyDVD metadata enrichment.

Looks up a movie's title, year, director, cast, genres and summary, and stores
them as metadata.json in a .tdvd archive or TinyDVD directory.

Sources, all used within their terms:
  - Wikidata (CC0, no key needed) for the facts, and the English Wikipedia
    summary (CC BY-SA, credited in the metadata) for the overview.
  - TMDb, if you give it your own API key (TMDB_API_KEY or --tmdb-key).
    "This product uses the TMDB API but is not endorsed or certified by TMDB."
  - OMDb, if you give it your own API key (OMDB_API_KEY or --omdb-key).

Only the movie title (and year, if known) is sent to these services.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import urllib.parse
import urllib.request

import tinydvd_archive

USER_AGENT = "TinyDVD/1.0 (https://github.com/boutell/tinydvd)"
HTTP_TIMEOUT = 10
MAX_IFO_BYTES = 16 * 1024 * 1024
MAX_CAST = 25

WIKIDATA_API = "https://www.wikidata.org/w/api.php"
WIKIPEDIA_SUMMARY = "https://en.wikipedia.org/api/rest_v1/page/summary/"
TMDB_API = "https://api.themoviedb.org/3"
OMDB_API = "https://www.omdbapi.com/"

# Wikidata classes a DVD's main feature is likely to be an instance of
WIKIDATA_FILM_CLASSES = {
    "Q11424",     # film
    "Q24869",     # feature film
    "Q202866",    # animated film
    "Q29168811",  # animated feature film
    "Q506240",    # television film
    "Q93204",     # documentary film
    "Q226730",    # silent film
    "Q5398426",   # television series
    "Q1259759",   # miniseries
}


# ---------------------------------------------------------------------------
# What the disc itself says
# ---------------------------------------------------------------------------

def _bcd_to_int(b: int) -> int:
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F)


def _ifo_names(package: str) -> list[str]:
    return sorted(n for n in tinydvd_archive.list_members(package) if n.upper().endswith(".IFO"))


def compute_dvd_runtime_minutes(package: str) -> int:
    """The longest PGC playback time in the disc's VTS_*.IFO files, in minutes."""
    max_sec = 0
    for name in _ifo_names(package):
        if not name.upper().startswith("VTS_"):
            continue
        data = tinydvd_archive.read_member(package, name, MAX_IFO_BYTES)
        if not data or len(data) < 0x100:
            continue
        pgci_sector = struct.unpack(">I", data[0xCC:0xD0])[0]
        base = pgci_sector * 2048
        if pgci_sector == 0 or base + 8 > len(data):
            continue
        num_pgcs = struct.unpack(">H", data[base:base + 2])[0]
        for i in range(num_pgcs):
            entry_off = base + 8 + i * 8
            if entry_off + 8 > len(data):
                break
            pgc_off = base + struct.unpack(">I", data[entry_off + 4:entry_off + 8])[0]
            if pgc_off + 8 > len(data):
                continue
            pb = data[pgc_off + 4:pgc_off + 8]
            h, m, s = _bcd_to_int(pb[0]), _bcd_to_int(pb[1]), _bcd_to_int(pb[2])
            if m < 60 and s < 60:
                max_sec = max(max_sec, h * 3600 + m * 60 + s)
    return round(max_sec / 60.0) if max_sec > 0 else 0


def compute_dvd_disc_id(package: str) -> str:
    """
    MD5 disc ID following libdvdread's DVDDiscID(): the whole contents of
    VIDEO_TS.IFO and VTS_01_0.IFO .. VTS_09_0.IFO, in that order, skipping
    any that don't exist.
    """
    md5 = hashlib.md5()
    found = False
    names = ["VIDEO_TS.IFO"] + [f"VTS_{n:02d}_0.IFO" for n in range(1, 10)]
    for name in names:
        data = tinydvd_archive.read_member(package, name, MAX_IFO_BYTES)
        if data:
            md5.update(data)
            found = True
    return md5.hexdigest() if found else ""


def clean_title_from_filename(filename: str) -> tuple[str, int | None]:
    """Title and optional year from a name like 'Dune (2021).tdvd'."""
    base = os.path.basename(filename.rstrip("/"))
    for ext in tinydvd_archive.TDVD_EXTENSIONS + (".iso", ".mkv"):
        if base.lower().endswith(ext):
            base = base[: -len(ext)]
            break
    base = base.replace("_", " ").strip()
    for pattern in (r"\((\d{4})\)", r"\b(19\d{2}|20\d{2})\b"):
        match = re.search(pattern, base)
        if match:
            return base[: match.start()].strip(), int(match.group(1))
    return base, None


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

def _get_json(url: str, params: dict | None = None) -> dict:
    if params:
        url = f"{url}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data if isinstance(data, dict) else {}


def _normalize_title(t: str) -> str:
    t = re.sub(r"[^\w\s]", " ", (t or "").lower())
    t = re.sub(r"^\s*(the|a|an)\s+", "", t)
    return " ".join(t.split())


# ---------------------------------------------------------------------------
# Wikidata + Wikipedia (no key)
# ---------------------------------------------------------------------------

def _label(entity: dict) -> str:
    """English label, or the language-neutral ("mul") one Wikidata now uses for many names."""
    labels = entity.get("labels", {})
    return (labels.get("en") or labels.get("mul") or {}).get("value", "")


def _aliases(entity: dict) -> list[str]:
    aliases = entity.get("aliases", {})
    return [a.get("value", "") for lang in ("en", "mul") for a in aliases.get(lang, [])]


def _claim_values(entity: dict, prop: str) -> list:
    out = []
    for claim in entity.get("claims", {}).get(prop, []):
        # Deprecated statements are known to be wrong, e.g. a release date
        # that was announced and then cancelled
        if claim.get("rank") == "deprecated":
            continue
        val = claim.get("mainsnak", {}).get("datavalue", {}).get("value")
        if val is not None:
            out.append(val)
    return out


def _claim_ids(entity: dict, prop: str) -> list[str]:
    return [v["id"] for v in _claim_values(entity, prop) if isinstance(v, dict) and "id" in v]


def _wikidata_year(entity: dict) -> int | None:
    years = []
    for v in _claim_values(entity, "P577"):
        m = re.match(r"^\+?(\d{4})-", str(v.get("time", ""))) if isinstance(v, dict) else None
        if m:
            years.append(int(m.group(1)))
    return min(years) if years else None


def _wikidata_runtimes(entity: dict) -> list[int]:
    """Every running time listed, in minutes (a film can have several cuts)"""
    out = []
    for v in _claim_values(entity, "P2047"):
        try:
            amount = float(str(v.get("amount", "0")))
        except (TypeError, ValueError):
            continue
        unit = str(v.get("unit", ""))
        if unit.endswith("/Q11574"):    # second
            amount /= 60.0
        elif unit.endswith("/Q25235"):  # hour
            amount *= 60.0
        if amount > 0:
            out.append(int(round(amount)))
    return out


def _wikidata_labels(ids: list[str]) -> dict[str, str]:
    labels: dict[str, str] = {}
    for i in range(0, len(ids), 50):
        chunk = ids[i:i + 50]
        data = _get_json(WIKIDATA_API, {
            "action": "wbgetentities", "ids": "|".join(chunk),
            "props": "labels", "languages": "en|mul", "format": "json",
        })
        for qid, ent in data.get("entities", {}).items():
            label = _label(ent)
            if label:
                labels[qid] = label
    return labels


def _wikipedia_summary(enwiki_title: str) -> str:
    if not enwiki_title:
        return ""
    data = _get_json(WIKIPEDIA_SUMMARY + urllib.parse.quote(enwiki_title.replace(" ", "_"), safe=""))
    return str(data.get("extract") or "").strip()


def _too_new(cand_year: int | None, max_year: int | None) -> int:
    """1 if a candidate film is newer than the disc it's on (so it can't be that film)"""
    return 1 if (max_year and cand_year and cand_year > max_year) else 0


# How close a film's running time must be to the disc's before a shortened
# title (see below) is trusted
SHORTENED_TITLE_RUNTIME_SLACK = 10


def fetch_wikidata_metadata(title: str, year: int | None = None, runtime_min: int = 0,
                            max_year: int | None = None) -> dict | None:
    """
    Look the title up on Wikidata. Returns None unless a film's title matches
    exactly, ignoring case and punctuation.

    Disc labels often end in junk ("DUNE_PART_TWO_OK", "ALIEN_WS", "..._D1"),
    so if nothing matches, it tries again without the last word or two. A
    match found that way must also fit the disc: its running time close to the
    main feature's, and not newer than the disc.
    """
    words = title.split()
    for drop in range(0, min(2, len(words) - 1) + 1):
        shortened = drop > 0
        if shortened and not runtime_min:
            break
        meta = _fetch_wikidata_exact(" ".join(words[:len(words) - drop]), year, runtime_min, max_year,
                                     must_fit=shortened)
        if meta:
            return meta
    return None


def _fetch_wikidata_exact(title: str, year: int | None, runtime_min: int, max_year: int | None,
                          must_fit: bool) -> dict | None:
    class_filter = "|".join(f"P31={q}" for q in sorted(WIKIDATA_FILM_CLASSES))
    found = _get_json(WIKIDATA_API, {
        "action": "query", "list": "search", "format": "json", "srlimit": "20",
        "srsearch": f"{title} haswbstatement:{class_filter}",
    })
    ids = [r["title"] for r in found.get("query", {}).get("search", []) if re.match(r"^Q\d+$", r.get("title", ""))]
    if not ids:
        return None

    data = _get_json(WIKIDATA_API, {
        "action": "wbgetentities", "ids": "|".join(ids),
        "props": "labels|aliases|claims|sitelinks", "languages": "en|mul", "format": "json",
    })
    wanted = _normalize_title(title)
    candidates = []
    for qid in ids:
        ent = data.get("entities", {}).get(qid)
        if not ent:
            continue
        names = [_label(ent)] + _aliases(ent)
        if wanted not in {_normalize_title(n) for n in names if n}:
            continue
        cand_year = _wikidata_year(ent)
        cand_rts = _wikidata_runtimes(ent)
        year_miss = 0 if (not year or (cand_year and abs(cand_year - year) <= 1)) else 1
        rt_diff = min((abs(rt - runtime_min) for rt in cand_rts), default=999) if runtime_min else 999
        popularity = len(ent.get("sitelinks", {}))
        # A year given outright wins; then a film newer than the disc is
        # ruled out; then the closest running time; then the best known
        too_new = _too_new(cand_year, max_year)
        if must_fit and (too_new or rt_diff > SHORTENED_TITLE_RUNTIME_SLACK):
            continue
        candidates.append(((year_miss, too_new, rt_diff, -popularity), ent, cand_year))
    if not candidates:
        return None
    candidates.sort(key=lambda c: c[0])
    _, ent, cand_year = candidates[0]

    director_ids = _claim_ids(ent, "P57")
    cast_ids = _claim_ids(ent, "P161")[:MAX_CAST]
    genre_ids = _claim_ids(ent, "P136")
    labels = _wikidata_labels(director_ids + cast_ids + genre_ids)
    genres = []
    for g in genre_ids:
        name = re.sub(r"\s+film$", "", labels.get(g, "")).strip()
        if name and name not in genres:
            genres.append(name)

    enwiki = ent.get("sitelinks", {}).get("enwiki", {}).get("title", "")
    overview = ""
    try:
        overview = _wikipedia_summary(enwiki)
    except Exception as exc:  # the facts are still worth keeping
        print(f"[warning] Wikipedia summary lookup failed: {exc}", file=sys.stderr)

    imdb = [v for v in _claim_values(ent, "P345") if isinstance(v, str)]
    meta = {
        "title": _label(ent) or title,
        "year": cand_year or year or 0,
        "director": ", ".join(labels[d] for d in director_ids if d in labels),
        "cast": ", ".join(labels[c] for c in cast_ids if c in labels),
        "genres": ", ".join(genres),
        "overview": overview,
        "wikidata_id": ent.get("id", ""),
        "imdb_id": imdb[0] if imdb else "",
        "source": "Wikidata (CC0)",
    }
    if overview and enwiki:
        meta["overview_source"] = "Wikipedia (CC BY-SA 4.0)"
        meta["overview_url"] = "https://en.wikipedia.org/wiki/" + urllib.parse.quote(enwiki.replace(" ", "_"))
    return meta


def fetch_wikidata_cast_by_imdb_id(imdb_id: str) -> list[str]:
    """Cast list for an IMDb title ID, from Wikidata."""
    if not re.match(r"^tt\d+$", imdb_id or ""):
        return []
    found = _get_json(WIKIDATA_API, {
        "action": "query", "list": "search", "format": "json", "srlimit": "1",
        "srsearch": f"haswbstatement:P345={imdb_id}",
    })
    hits = found.get("query", {}).get("search", [])
    if not hits:
        return []
    data = _get_json(WIKIDATA_API, {
        "action": "wbgetentities", "ids": hits[0]["title"], "props": "claims", "format": "json",
    })
    ent = next(iter(data.get("entities", {}).values()), {})
    cast_ids = _claim_ids(ent, "P161")[:MAX_CAST]
    labels = _wikidata_labels(cast_ids)
    return [labels[c] for c in cast_ids if c in labels]


# ---------------------------------------------------------------------------
# TMDb (your own key)
# ---------------------------------------------------------------------------

def fetch_tmdb_metadata(title: str, year: int | None = None, api_key: str = "", runtime_min: int = 0,
                        max_year: int | None = None) -> dict | None:
    """Look the title up on The Movie Database with the user's API key."""
    if not api_key:
        return None
    params = {"api_key": api_key, "query": title}
    if year:
        params["year"] = str(year)
    results = _get_json(f"{TMDB_API}/search/movie", params).get("results", [])
    if not results:
        return None
    # Films newer than the disc can't be on it (if that leaves any)
    if not year and max_year:
        def rel_year(r: dict) -> int | None:
            d = str(r.get("release_date") or "")
            return int(d[:4]) if d[:4].isdigit() else None
        older = [r for r in results if not _too_new(rel_year(r), max_year)]
        results = older or results

    movie = results[0]
    details = None
    # Several films share the exact title and we don't know the year: pick
    # the one whose runtime is closest to the disc's main feature
    if year is None and runtime_min > 0 and len(results) > 1:
        wanted = title.strip().lower()
        exact = [r for r in results if r.get("title", "").strip().lower() == wanted]
        if len(exact) > 1:
            top_votes = exact[0].get("vote_count", 0) or 0
            best_diff = None
            for cand in exact[:5]:
                if top_votes >= 100 and (cand.get("vote_count", 0) or 0) < 100:
                    continue
                cand_det = _get_json(f"{TMDB_API}/movie/{cand['id']}", {"api_key": api_key})
                cand_rt = cand_det.get("runtime") or 0
                diff = abs(cand_rt - runtime_min) if cand_rt > 0 else 99999
                if best_diff is None or diff < best_diff:
                    best_diff, movie, details = diff, cand, cand_det

    movie_id = movie["id"]
    credits = _get_json(f"{TMDB_API}/movie/{movie_id}/credits", {"api_key": api_key})
    if details is None:
        details = _get_json(f"{TMDB_API}/movie/{movie_id}", {"api_key": api_key})

    rel_date = movie.get("release_date", "")
    return {
        "title": movie.get("title", title),
        "year": int(rel_date[:4]) if rel_date[:4].isdigit() else (year or 0),
        "director": ", ".join(c["name"] for c in credits.get("crew", []) if c.get("job") == "Director"),
        "cast": ", ".join(c["name"] for c in credits.get("cast", [])[:MAX_CAST]),
        "genres": ", ".join(g["name"] for g in details.get("genres", [])),
        "overview": movie.get("overview", ""),
        "rating": movie.get("vote_average", 0.0),
        "tmdb_id": movie_id,
        "poster_path": movie.get("poster_path", ""),
        "source": "TMDb (this product uses the TMDB API but is not endorsed or certified by TMDB)",
    }


# ---------------------------------------------------------------------------
# OMDb (your own key)
# ---------------------------------------------------------------------------

def _parse_omdb_item(data: dict, title: str, year: int | None) -> tuple[dict | None, int, int]:
    if not data or data.get("Response") != "True":
        return None, 0, 0
    y_str = data.get("Year", "")
    rt_tok = (str(data.get("Runtime", "")).split() or [""])[0]
    votes = str(data.get("imdbVotes", "")).replace(",", "").strip()
    rating = str(data.get("imdbRating", ""))
    meta = {
        "title": data.get("Title", title),
        "year": int(y_str[:4]) if y_str[:4].isdigit() else (year or 0),
        "director": data.get("Director", ""),
        "cast": data.get("Actors", ""),
        "genres": data.get("Genre", ""),
        "overview": data.get("Plot", ""),
        "rating": float(rating) if rating.replace(".", "", 1).isdigit() else 0.0,
        "imdb_id": data.get("imdbID", ""),
        "source": "OMDb",
    }
    return meta, (int(rt_tok) if rt_tok.isdigit() else 0), (int(votes) if votes.isdigit() else 0)


def fetch_omdb_metadata(title: str, year: int | None = None, api_key: str = "", runtime_min: int = 0,
                        max_year: int | None = None) -> dict | None:
    """Look the title up on OMDb with the user's API key."""
    if not api_key:
        return None
    params = {"t": title, "type": "movie", "apikey": api_key}
    if year:
        params["y"] = str(year)
    primary, primary_rt, primary_votes = _parse_omdb_item(_get_json(OMDB_API, params), title, year)
    selected = primary

    primary_too_new = bool(primary and _too_new(primary.get("year"), max_year))
    if year is None and (runtime_min > 0 or primary_too_new):
        sdata = _get_json(OMDB_API, {"s": title, "type": "movie", "apikey": api_key})
        wanted = title.strip().lower()
        exact = [c for c in sdata.get("Search", []) if c.get("Title", "").strip().lower() == wanted]
        if len(exact) > 1 or (primary is None and exact):
            best_diff = abs(primary_rt - runtime_min) if (primary and primary_rt > 0 and runtime_min) else 99999
            if primary_too_new:
                selected, best_diff = None, 99999
            for cand in exact[:5]:
                if not cand.get("imdbID"):
                    continue
                cmeta, crt, cvotes = _parse_omdb_item(
                    _get_json(OMDB_API, {"i": cand["imdbID"], "apikey": api_key}), title, year)
                if not cmeta or _too_new(cmeta.get("year"), max_year):
                    continue
                if primary_votes >= 1000 and cvotes < 1000 and not primary_too_new:
                    continue
                diff = abs(crt - runtime_min) if (crt > 0 and runtime_min) else 99998
                if selected is None or diff < best_diff:
                    best_diff, selected = diff, cmeta
        if selected is None:
            selected = primary  # nothing older found: the disc's date may be wrong

    # OMDb lists only the top three or four actors
    if selected and selected.get("imdb_id"):
        try:
            names = [n.strip() for n in selected.get("cast", "").split(",") if n.strip() and n.strip() != "N/A"]
            for n in fetch_wikidata_cast_by_imdb_id(selected["imdb_id"]):
                if n.lower() not in {x.lower() for x in names}:
                    names.append(n)
            selected["cast"] = ", ".join(names[:MAX_CAST])
        except Exception as exc:
            print(f"[warning] Wikidata cast lookup failed: {exc}", file=sys.stderr)
    return selected


# ---------------------------------------------------------------------------
# Putting it together
# ---------------------------------------------------------------------------

def lookup_metadata(title: str, year: int | None, runtime_min: int, tmdb_key: str = "", omdb_key: str = "",
                    max_year: int | None = None) -> dict | None:
    """
    Try each source in turn: TMDb and OMDb if keyed, then Wikidata. Among
    films with the same title: a given year wins; failing that, films newer
    than max_year (the year the disc was mastered) are passed over, then the
    running time closest to runtime_min decides.
    """
    sources = []
    if tmdb_key:
        sources.append(("TMDb", lambda: fetch_tmdb_metadata(title, year, tmdb_key, runtime_min, max_year)))
    if omdb_key:
        sources.append(("OMDb", lambda: fetch_omdb_metadata(title, year, omdb_key, runtime_min, max_year)))
    sources.append(("Wikidata", lambda: fetch_wikidata_metadata(title, year, runtime_min, max_year)))
    for name, fetch in sources:
        print(f"  - Looking up '{title}' on {name}...")
        try:
            meta = fetch()
        except Exception as exc:
            print(f"[warning] {name} lookup failed: {exc}", file=sys.stderr)
            continue
        if meta:
            return meta
    return None


def write_metadata(package: str, meta: dict, write_sidecar: bool = False) -> None:
    """Store metadata.json in the package, and in a sidecar .json if asked or one exists."""
    data = json.dumps(meta, indent=2, ensure_ascii=False).encode("utf-8")
    tinydvd_archive.write_members(package, {tinydvd_archive.METADATA_NAME: data})
    print(f"[success] Saved {tinydvd_archive.METADATA_NAME} in {package}")
    if os.path.isfile(package):
        sidecar = tinydvd_archive.sidecar_metadata_path(package)
        if write_sidecar or os.path.isfile(sidecar):
            tinydvd_archive.write_file_atomically(sidecar, data)
            print(f"[success] Wrote sidecar metadata to {sidecar}")


def enrich_movie(package: str, title: str | None = None, year: int | None = None,
                 tmdb_key: str = "", omdb_key: str = "", title_hint_path: str | None = None,
                 overrides: dict | None = None, write: bool = True, sidecar: bool = False,
                 mastered: str = "") -> dict:
    """
    Build metadata for a package: look it up online, then apply any overrides
    (director, cast, genres, overview). Returns the metadata, and saves it
    into the package unless write is False.
    """
    clean_t, clean_y = clean_title_from_filename(title_hint_path or package)
    existing = tinydvd_archive.read_metadata(package)
    cur_title = title or clean_t or existing.get("title") or "Unknown Movie"
    cur_year = year or clean_y

    print(f"Enriching: '{cur_title}' ({cur_year or 'unknown year'}) in {package}...")
    disc_id = compute_dvd_disc_id(package)
    runtime_min = compute_dvd_runtime_minutes(package)
    if runtime_min > 0:
        print(f"  - Main feature runtime: ~{runtime_min} min")
    # When the disc was mastered (kept from the import, since a package
    # can't tell): the film can't be newer
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", mastered or ""):
        mastered = str(existing.get("disc_mastered") or "")
    max_year = int(mastered[:4]) if re.fullmatch(r"\d{4}-\d{2}-\d{2}", mastered) else None
    if max_year:
        print(f"  - Disc mastered: {mastered}")

    meta = lookup_metadata(cur_title, cur_year, runtime_min, tmdb_key, omdb_key, max_year)
    if not meta:
        print("  - No match found online; keeping existing or given values.")
        meta = {
            "title": cur_title,
            "year": cur_year or existing.get("year", 0),
            "director": existing.get("director", ""),
            "cast": existing.get("cast", ""),
            "genres": existing.get("genres", ""),
            "overview": existing.get("overview", ""),
        }
    meta["disc_id"] = disc_id or existing.get("disc_id", "")
    if max_year:
        meta["disc_mastered"] = mastered
    for key, val in (overrides or {}).items():
        if val:
            meta[key] = val

    print(f"  - Title:    {meta.get('title')}")
    print(f"  - Year:     {meta.get('year')}")
    print(f"  - Director: {meta.get('director')}")
    print(f"  - Cast:     {str(meta.get('cast', ''))[:60]}")
    print(f"  - Genres:   {meta.get('genres')}")

    if write:
        write_metadata(package, meta, sidecar)
    return meta


def main() -> None:
    parser = argparse.ArgumentParser(description="Look up and store movie metadata in TinyDVD packages")
    parser.add_argument("target", help="A .tdvd file, a TinyDVD directory, or a folder of .tdvd files")
    parser.add_argument("--title", help="Override movie title")
    parser.add_argument("--year", type=int, help="Override release year")
    parser.add_argument("--director", help="Override director")
    parser.add_argument("--cast", help="Override cast")
    parser.add_argument("--genres", help="Override genres")
    parser.add_argument("--overview", help="Override overview")
    parser.add_argument("--tmdb-key", default=os.environ.get("TMDB_API_KEY", ""),
                        help="Your TMDb API key (or set TMDB_API_KEY)")
    parser.add_argument("--omdb-key", default=os.environ.get("OMDB_API_KEY", ""),
                        help="Your OMDb API key (or set OMDB_API_KEY)")
    parser.add_argument("--sidecar", action="store_true", help="Also write a .json file next to the archive")
    args = parser.parse_args()

    overrides = {"director": args.director, "cast": args.cast, "genres": args.genres, "overview": args.overview}

    def enrich(path: str) -> None:
        enrich_movie(path, title=args.title, year=args.year, tmdb_key=args.tmdb_key, omdb_key=args.omdb_key,
                     overrides=overrides, sidecar=args.sidecar)

    is_disc_dir = any(n.upper() == "VIDEO_TS.IFO" for n in tinydvd_archive.list_members(args.target))
    if os.path.isdir(args.target) and not is_disc_dir:
        for root, _, files in os.walk(args.target):
            for name in sorted(files):
                if tinydvd_archive.is_tdvd_path(name):
                    enrich(os.path.join(root, name))
    else:
        enrich(args.target)


if __name__ == "__main__":
    main()

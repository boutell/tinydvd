#!/usr/bin/env python3
"""
TinyDVD Metadata Enrichment & Scraper Tool
Fetches movie details (director, cast, genres, overview, year, ratings, TMDb ID)
and embeds them as metadata.json into .tdvd archives or writes sidecar .json files.
"""

import sys
import os
import re
import json
import zipfile
import hashlib
import struct
import argparse
import urllib.request
import urllib.parse

TMDB_API_KEY_DEFAULT = os.environ.get("TMDB_API_KEY", "")


def _bcd_to_int(b: int) -> int:
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F)


def compute_dvd_runtime_minutes(target_path: str) -> int:
    """Extract the longest PGC playback duration (in rounded minutes) from VTS_*.IFO files."""
    max_sec = 0
    ifo_blobs: list[bytes] = []
    try:
        if os.path.isdir(target_path):
            for fname in sorted(os.listdir(target_path)):
                upper = fname.upper()
                if upper.startswith("VTS_") and upper.endswith(".IFO"):
                    with open(os.path.join(target_path, fname), "rb") as f:
                        ifo_blobs.append(f.read())
        elif os.path.isfile(target_path):
            with zipfile.ZipFile(target_path, "r") as zf:
                for name in sorted(zf.namelist()):
                    upper = name.upper()
                    if upper.startswith("VTS_") and upper.endswith(".IFO"):
                        ifo_blobs.append(zf.read(name))
    except Exception:
        return 0

    for data in ifo_blobs:
        if len(data) < 0x100:
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
            h = _bcd_to_int(pb[0])
            m = _bcd_to_int(pb[1])
            s = _bcd_to_int(pb[2])
            if m < 60 and s < 60:
                sec = h * 3600 + m * 60 + s
                if sec > max_sec:
                    max_sec = sec

    return round(max_sec / 60.0) if max_sec > 0 else 0

def compute_dvd_disc_id(target_path: str) -> str:
    """Compute 128-bit MD5 disc ID from IFO files matching libdvdread's DVDDiscID."""
    md5 = hashlib.md5()
    try:
        if os.path.isdir(target_path):
            ifo_files = sorted([f for f in os.listdir(target_path) if f.upper().endswith('.IFO')])
            for name in ifo_files:
                with open(os.path.join(target_path, name), 'rb') as f:
                    md5.update(f.read(128))
            return md5.hexdigest()
        with zipfile.ZipFile(target_path, 'r') as zf:
            ifo_names = sorted([n for n in zf.namelist() if n.upper().endswith('.IFO')])
            for name in ifo_names:
                data = zf.read(name)[:128] # libdvdread hashes first 128 bytes of each IFO
                md5.update(data)
        return md5.hexdigest()
    except Exception:
        return ""

def clean_title_from_filename(filename: str) -> tuple[str, int | None]:
    """Extract clean title and optional year from filename like 'Dune (2021).tdvd'."""
    base = os.path.basename(filename)
    for ext in ('.tdvd', '.tinydvd', '.iso', '.mkv'):
        if base.lower().endswith(ext):
            base = base[:-len(ext)]
            break
    base = base.replace('_', ' ').strip()
    
    # Check for (YYYY) in title
    import re
    match = re.search(r'\((\d{4})\)', base)
    if match:
        year = int(match.group(1))
        title = base[:match.start()].strip()
        return title, year
    
    # Check for trailing YYYY
    match = re.search(r'\b(19\d{2}|20\d{2})\b', base)
    if match:
        year = int(match.group(1))
        title = base[:match.start()].strip()
        return title, year
        
    return base, None

def fetch_tmdb_metadata(title: str, year: int | None = None, api_key: str = "", runtime_min: int = 0) -> dict | None:
    """Fetch rich metadata from The Movie Database (TMDb) API."""
    if not api_key:
        return None
    try:
        query_params = {"api_key": api_key, "query": title}
        if year:
            query_params["year"] = str(year)
        url = f"https://api.themoviedb.org/3/search/movie?{urllib.parse.urlencode(query_params)}"
        req = urllib.request.Request(url, headers={"User-Agent": "TinyDVD/1.0"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode('utf-8'))
            results = data.get("results", [])
            if not results:
                return None

        movie = results[0]
        ddata_cached = None

        if year is None and runtime_min > 0 and len(results) > 1:
            t_low = title.strip().lower()
            exact = [r for r in results if r.get("title", "").strip().lower() == t_low]
            if len(exact) > 1:
                top_votes = exact[0].get("vote_count", 0) or 0
                best_diff = None
                for cand in exact[:5]:
                    cvotes = cand.get("vote_count", 0) or 0
                    if top_votes >= 100 and cvotes < 100:
                        continue
                    cid = cand["id"]
                    det_url = f"https://api.themoviedb.org/3/movie/{cid}?api_key={api_key}"
                    det_req = urllib.request.Request(det_url, headers={"User-Agent": "TinyDVD/1.0"})
                    with urllib.request.urlopen(det_req, timeout=10) as resp:
                        cand_det = json.loads(resp.read().decode('utf-8'))
                    cand_rt = cand_det.get("runtime") or 0
                    diff = abs(cand_rt - runtime_min) if cand_rt > 0 else 99999
                    if best_diff is None or diff < best_diff:
                        best_diff = diff
                        movie = cand
                        ddata_cached = cand_det

        movie_id = movie["id"]

        # Fetch credits for director and top cast
        credits_url = f"https://api.themoviedb.org/3/movie/{movie_id}/credits?api_key={api_key}"
        creds_req = urllib.request.Request(credits_url, headers={"User-Agent": "TinyDVD/1.0"})
        with urllib.request.urlopen(creds_req, timeout=10) as resp:
            cdata = json.loads(resp.read().decode('utf-8'))

        directors = [c["name"] for c in cdata.get("crew", []) if c.get("job") == "Director"]
        cast = [c["name"] for c in cdata.get("cast", [])[:25]]

        # Fetch details for genres if not cached during runtime disambiguation
        if ddata_cached is not None:
            ddata = ddata_cached
        else:
            details_url = f"https://api.themoviedb.org/3/movie/{movie_id}?api_key={api_key}"
            det_req = urllib.request.Request(details_url, headers={"User-Agent": "TinyDVD/1.0"})
            with urllib.request.urlopen(det_req, timeout=10) as resp:
                ddata = json.loads(resp.read().decode('utf-8'))

        genres = [g["name"] for g in ddata.get("genres", [])]
        rel_date = movie.get("release_date", "")
        rel_year = int(rel_date.split("-")[0]) if rel_date and "-" in rel_date else year

        return {
            "title": movie.get("title", title),
            "year": rel_year,
            "director": ", ".join(directors),
            "cast": ", ".join(cast),
            "genres": ", ".join(genres),
            "overview": movie.get("overview", ""),
            "rating": movie.get("vote_average", 0.0),
            "tmdb_id": movie_id,
            "poster_path": movie.get("poster_path", "")
        }
    except Exception as e:
        print(f"[warning] TMDb lookup error: {e}", file=sys.stderr)
        return None


def _parse_omdb_item(data: dict, title: str, year: int | None) -> tuple[dict | None, int, int]:
    if not data or data.get("Response") != "True":
        return None, 0, 0
    y_str = data.get("Year", "")
    parsed_year = int(y_str[:4]) if len(y_str) >= 4 and y_str[:4].isdigit() else year
    rt_raw = str(data.get("Runtime", ""))
    rt_tok = rt_raw.split()[0] if rt_raw.split() else ""
    parsed_runtime = int(rt_tok) if rt_tok.isdigit() else 0
    votes_raw = str(data.get("imdbVotes", "")).replace(",", "").strip()
    parsed_votes = int(votes_raw) if votes_raw.isdigit() else 0
    meta = {
        "title": data.get("Title", title),
        "year": parsed_year,
        "director": data.get("Director", ""),
        "cast": data.get("Actors", ""),
        "genres": data.get("Genre", ""),
        "overview": data.get("Plot", ""),
        "rating": float(data.get("imdbRating", 0.0)) if data.get("imdbRating", "").replace('.', '', 1).isdigit() else 0.0,
        "imdb_id": data.get("imdbID", "")
    }
    return meta, parsed_runtime, parsed_votes


def fetch_extended_cast_by_imdb_id(imdb_id: str, initial_cast: str = "") -> str:
    """Expand OMDb's 3-4 person Actors summary using Wikidata SPARQL and IMDb page JSON."""
    names: list[str] = []
    seen: set[str] = set()

    def add_name(raw: str) -> None:
        if len(names) >= 25 or not raw:
            return
        clean = str(raw).strip()
        if not clean or clean.lower() == "n/a":
            return
        key = clean.lower()
        if key not in seen:
            seen.add(key)
            names.append(clean)

    for part in (initial_cast or "").split(","):
        add_name(part)

    if not imdb_id or not re.match(r"^tt\d+$", imdb_id.strip()):
        return ", ".join(names[:25])
    imdb_id = imdb_id.strip()

    # Source 1: Wikidata SPARQL cast query by IMDb ID (wdt:P345 -> wdt:P161)
    try:
        sparql = (
            "SELECT ?actorLabel WHERE {\n"
            f'  ?movie wdt:P345 "{imdb_id}".\n'
            "  ?movie wdt:P161 ?actor.\n"
            '  SERVICE wikibase:label { bd:serviceParam wikibase:language "en,mul". }\n'
            "} LIMIT 30"
        )
        wd_url = "https://query.wikidata.org/sparql?" + urllib.parse.urlencode(
            {"query": sparql, "format": "json"}
        )
        wd_req = urllib.request.Request(
            wd_url,
            headers={
                "User-Agent": "TinyDVD/1.0 (https://github.com/boutell/tinydvd)",
                "Accept": "application/sparql-results+json",
            },
        )
        with urllib.request.urlopen(wd_req, timeout=8) as resp:
            wd_data = json.loads(resp.read().decode("utf-8"))
        for binding in wd_data.get("results", {}).get("bindings", []):
            val = binding.get("actorLabel", {}).get("value", "")
            if val and not re.match(r"^Q\d+$", val):
                add_name(val)
    except Exception:
        pass

    # Source 2: IMDb title page JSON (__NEXT_DATA__ cast edges / JSON-LD actor list)
    if len(names) <= 4:
        try:
            imdb_url = f"https://www.imdb.com/title/{imdb_id}/"
            imdb_req = urllib.request.Request(
                imdb_url,
                headers={
                    "User-Agent": (
                        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
                    ),
                    "Accept-Language": "en-US,en;q=0.9",
                },
            )
            with urllib.request.urlopen(imdb_req, timeout=8) as resp:
                html = resp.read().decode("utf-8", errors="ignore")

            nd_match = re.search(
                r'<script[^>]*id=["\']__NEXT_DATA__["\'][^>]*>(.*?)</script>',
                html,
                re.S | re.I,
            )
            if nd_match:
                try:
                    nd = json.loads(nd_match.group(1))
                    stack = [nd]
                    while stack and len(names) < 25:
                        cur = stack.pop()
                        if isinstance(cur, dict):
                            cast_val = cur.get("cast")
                            if isinstance(cast_val, dict) and isinstance(cast_val.get("edges"), list):
                                for edge in cast_val["edges"]:
                                    n = (
                                        edge.get("node", {})
                                        .get("name", {})
                                        .get("nameText", {})
                                        .get("text", "")
                                    )
                                    add_name(n)
                            for v in cur.values():
                                if isinstance(v, (dict, list)):
                                    stack.append(v)
                        elif isinstance(cur, list):
                            for item in cur:
                                if isinstance(item, (dict, list)):
                                    stack.append(item)
                except Exception:
                    pass

            if len(names) <= 4:
                for ld_match in re.finditer(
                    r'<script[^>]*type=["\']application/ld\+json["\'][^>]*>(.*?)</script>',
                    html,
                    re.S | re.I,
                ):
                    try:
                        ld = json.loads(ld_match.group(1))
                        actors = ld.get("actor", []) if isinstance(ld, dict) else []
                        if isinstance(actors, dict):
                            actors = [actors]
                        for act in actors:
                            if isinstance(act, dict) and act.get("name"):
                                add_name(act["name"])
                    except Exception:
                        pass
        except Exception:
            pass

    return ", ".join(names[:25])


def fetch_open_metadata(title: str, year: int | None = None, runtime_min: int = 0) -> dict | None:
    """Fetch public metadata from free endpoints (e.g. OMDb free tiers, open scrapers)."""
    try:
        # Canonical title lookup first
        params = {"t": title, "type": "movie", "apikey": "trilogy"}
        if year:
            params["y"] = str(year)
        url = f"http://www.omdbapi.com/?{urllib.parse.urlencode(params)}"
        req = urllib.request.Request(url, headers={"User-Agent": "TinyDVD/1.0"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            pdata = json.loads(resp.read().decode('utf-8'))
            primary_meta, primary_rt, primary_votes = _parse_omdb_item(pdata, title, year)

        selected_meta = primary_meta

        if year is None and runtime_min > 0:
            search_params = {"s": title, "type": "movie", "apikey": "trilogy"}
            search_url = f"http://www.omdbapi.com/?{urllib.parse.urlencode(search_params)}"
            sreq = urllib.request.Request(search_url, headers={"User-Agent": "TinyDVD/1.0"})
            with urllib.request.urlopen(sreq, timeout=10) as resp:
                sdata = json.loads(resp.read().decode('utf-8'))
            search_list = sdata.get("Search", []) if sdata.get("Response") == "True" else []
            t_low = title.strip().lower()
            exact = [c for c in search_list if c.get("Title", "").strip().lower() == t_low]
            if len(exact) > 1 or (primary_meta is None and len(exact) >= 1):
                best_meta = primary_meta
                best_diff = abs(primary_rt - runtime_min) if (primary_meta and primary_rt > 0) else 99999
                for cand in exact[:5]:
                    imdb_id = cand.get("imdbID")
                    if not imdb_id:
                        continue
                    iurl = f"http://www.omdbapi.com/?{urllib.parse.urlencode({'i': imdb_id, 'apikey': 'trilogy'})}"
                    ireq = urllib.request.Request(iurl, headers={"User-Agent": "TinyDVD/1.0"})
                    with urllib.request.urlopen(ireq, timeout=10) as iresp:
                        idata = json.loads(iresp.read().decode('utf-8'))
                    cmeta, crt, cvotes = _parse_omdb_item(idata, title, year)
                    if not cmeta:
                        continue
                    if primary_votes >= 1000 and cvotes < 1000:
                        continue
                    diff = abs(crt - runtime_min) if crt > 0 else 99999
                    if best_meta is None or diff < best_diff:
                        best_diff = diff
                        best_meta = cmeta
                if best_meta:
                    selected_meta = best_meta

        if selected_meta and selected_meta.get("imdb_id"):
            selected_meta["cast"] = fetch_extended_cast_by_imdb_id(
                selected_meta["imdb_id"], selected_meta.get("cast", "")
            )
        return selected_meta
    except Exception:
        pass
    return None

def write_metadata_to_tdvd(tdvd_path: str, meta: dict, write_sidecar: bool = False):
    """Embed metadata.json into .tdvd archive and/or write sidecar .json or directory metadata.json."""
    data_bytes = json.dumps(meta, indent=2, ensure_ascii=False).encode('utf-8')

    if os.path.isdir(tdvd_path):
        meta_file = os.path.join(tdvd_path, 'metadata.json')
        with open(meta_file, 'wb') as f:
            f.write(data_bytes)
        print(f"[success] Wrote metadata.json into directory {tdvd_path}")
        return

    lower_path = tdvd_path.lower()
    if os.path.isfile(tdvd_path) and (lower_path.endswith('.tdvd') or lower_path.endswith('.tinydvd')):
        with zipfile.ZipFile(tdvd_path, 'a', allowZip64=True) as zf:
            matches = [info for info in zf.filelist if info.filename.lower() == 'metadata.json']
            for m in matches:
                zf.filelist.remove(m)
                if m.filename in zf.NameToInfo:
                    del zf.NameToInfo[m.filename]

            zinfo = zipfile.ZipInfo(filename='metadata.json')
            zinfo.compress_type = zipfile.ZIP_STORED
            zinfo.file_size = len(data_bytes)
            zinfo.external_attr = 0o644 << 16
            zf.writestr(zinfo, data_bytes)
        print(f"[success] Embedded metadata.json into {tdvd_path}")

    base = os.path.splitext(tdvd_path)[0]
    sidecar_path = f"{base}.json"
    if write_sidecar or not lower_path.endswith('.tdvd') or os.path.isfile(sidecar_path):
        with open(sidecar_path, 'w', encoding='utf-8') as f:
            f.write(data_bytes.decode('utf-8'))
        print(f"[success] Wrote sidecar metadata to {sidecar_path}")

def enrich_movie(tdvd_path: str, args=None, title: str | None = None, year: int | None = None,
                 tmdb_key: str = "", sidecar: bool = False, title_hint_path: str | None = None):
    naming_path = title_hint_path if title_hint_path else tdvd_path
    clean_t, clean_y = clean_title_from_filename(naming_path)

    arg_title = getattr(args, 'title', None) or title or clean_t
    arg_year = getattr(args, 'year', None) or year or clean_y
    arg_tmdb_key = getattr(args, 'tmdb_key', "") or tmdb_key or TMDB_API_KEY_DEFAULT
    arg_sidecar = getattr(args, 'sidecar', False) or sidecar
    arg_director = getattr(args, 'director', None)
    arg_cast = getattr(args, 'cast', None)
    arg_genres = getattr(args, 'genres', None)
    arg_overview = getattr(args, 'overview', None)

    # Check for existing metadata inside archive or directory
    existing_meta = {}
    if os.path.isfile(tdvd_path) and (tdvd_path.endswith('.tdvd') or tdvd_path.endswith('.tinydvd')):
        try:
            with zipfile.ZipFile(tdvd_path, 'r') as zf:
                if 'metadata.json' in zf.namelist():
                    existing_meta = json.loads(zf.read('metadata.json').decode('utf-8'))
        except Exception:
            pass
    elif os.path.isdir(tdvd_path):
        mpath = os.path.join(tdvd_path, 'metadata.json')
        if os.path.isfile(mpath):
            try:
                with open(mpath, 'r', encoding='utf-8') as mf:
                    existing_meta = json.load(mf)
            except Exception:
                pass

    cur_title = arg_title or existing_meta.get('title') or "Unknown Movie"
    cur_year = arg_year

    print(f"Enriching: '{cur_title}' ({cur_year or 'unknown year'}) in {tdvd_path}...")

    disc_id = compute_dvd_disc_id(tdvd_path)
    if disc_id:
        print(f"  - Computed DVD Disc ID: {disc_id}")

    runtime_min = compute_dvd_runtime_minutes(tdvd_path)
    if runtime_min > 0:
        print(f"  - Detected Main Feature Runtime: ~{runtime_min} min")

    meta = None
    if arg_tmdb_key:
        print("  - Querying TMDb API...")
        meta = fetch_tmdb_metadata(cur_title, cur_year, arg_tmdb_key, runtime_min=runtime_min)

    if not meta:
        print("  - Querying Open Movie Database...")
        meta = fetch_open_metadata(cur_title, cur_year, runtime_min=runtime_min)

    if not meta:
        print("  - Could not fetch remote metadata; using existing or CLI provided values.")
        meta = {
            "title": cur_title or existing_meta.get("title", ""),
            "year": cur_year or existing_meta.get("year", 0),
            "director": arg_director or existing_meta.get("director", ""),
            "cast": arg_cast or existing_meta.get("cast", ""),
            "genres": arg_genres or existing_meta.get("genres", ""),
            "overview": arg_overview or existing_meta.get("overview", ""),
            "disc_id": disc_id or existing_meta.get("disc_id", "")
        }
    else:
        meta["disc_id"] = disc_id
        if arg_director: meta["director"] = arg_director
        if arg_cast: meta["cast"] = arg_cast
        if arg_genres: meta["genres"] = arg_genres
        if arg_overview: meta["overview"] = arg_overview

    print(f"  - Title:    {meta.get('title')}")
    print(f"  - Year:     {meta.get('year')}")
    print(f"  - Director: {meta.get('director')}")
    print(f"  - Cast:     {str(meta.get('cast', ''))[:60]}...")
    print(f"  - Genres:   {meta.get('genres')}")

    write_metadata_to_tdvd(tdvd_path, meta, arg_sidecar)
    return meta

def main():
    parser = argparse.ArgumentParser(description="TinyDVD Metadata Scraper & Enrichment Tool")
    parser.add_argument("target", help="Path to .tdvd file or directory containing .tdvd files")
    parser.add_argument("--title", help="Override movie title")
    parser.add_argument("--year", type=int, help="Override movie release year")
    parser.add_argument("--director", help="Override movie director")
    parser.add_argument("--cast", help="Override movie cast")
    parser.add_argument("--genres", help="Override movie genres")
    parser.add_argument("--overview", help="Override movie overview")
    parser.add_argument("--tmdb-key", default=TMDB_API_KEY_DEFAULT, help="TMDb API v3 key (or set TMDB_API_KEY)")
    parser.add_argument("--sidecar", action="store_true", help="Always write sidecar .json alongside the file")
    args = parser.parse_args()

    if os.path.isdir(args.target):
        for root, _, files in os.walk(args.target):
            for file in sorted(files):
                if file.endswith('.tdvd') or file.endswith('.tinydvd'):
                    enrich_movie(os.path.join(root, file), args)
    else:
        enrich_movie(args.target, args)

if __name__ == "__main__":
    main()

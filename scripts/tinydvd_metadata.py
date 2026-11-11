#!/usr/bin/env python3
"""
TinyDVD Metadata Enrichment & Scraper Tool
Fetches movie details (director, cast, genres, overview, year, ratings, TMDb ID)
and embeds them as metadata.json into .tdvd archives or writes sidecar .json files.
"""

import sys
import os
import json
import zipfile
import hashlib
import argparse
import urllib.request
import urllib.parse

TMDB_API_KEY_DEFAULT = os.environ.get("TMDB_API_KEY", "")

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

def fetch_tmdb_metadata(title: str, year: int | None = None, api_key: str = "") -> dict | None:
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
            movie_id = movie["id"]

        # Fetch credits for director and top cast
        credits_url = f"https://api.themoviedb.org/3/movie/{movie_id}/credits?api_key={api_key}"
        creds_req = urllib.request.Request(credits_url, headers={"User-Agent": "TinyDVD/1.0"})
        with urllib.request.urlopen(creds_req, timeout=10) as resp:
            cdata = json.loads(resp.read().decode('utf-8'))

        directors = [c["name"] for c in cdata.get("crew", []) if c.get("job") == "Director"]
        cast = [c["name"] for c in cdata.get("cast", [])[:10]]

        # Fetch details for genres
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

def fetch_open_metadata(title: str, year: int | None = None) -> dict | None:
    """Fetch public metadata from free endpoints (e.g. OMDb free tiers, open scrapers)."""
    try:
        # Free public OMDb query (fallback demo key)
        params = {"t": title}
        if year:
            params["y"] = str(year)
        params["apikey"] = "trilogy" # widely available public test key
        url = f"http://www.omdbapi.com/?{urllib.parse.urlencode(params)}"
        req = urllib.request.Request(url, headers={"User-Agent": "TinyDVD/1.0"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode('utf-8'))
            if data.get("Response") == "True":
                y_str = data.get("Year", "")
                parsed_year = int(y_str[:4]) if len(y_str) >= 4 and y_str[:4].isdigit() else year
                return {
                    "title": data.get("Title", title),
                    "year": parsed_year,
                    "director": data.get("Director", ""),
                    "cast": data.get("Actors", ""),
                    "genres": data.get("Genre", ""),
                    "overview": data.get("Plot", ""),
                    "rating": float(data.get("imdbRating", 0.0)) if data.get("imdbRating", "").replace('.','',1).isdigit() else 0.0,
                    "imdb_id": data.get("imdbID", "")
                }
    except Exception as e:
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
    
    if os.path.isfile(tdvd_path) and (tdvd_path.endswith('.tdvd') or tdvd_path.endswith('.tinydvd')):
        needs_rewrite = False
        with zipfile.ZipFile(tdvd_path, 'r') as zf:
            if 'metadata.json' in zf.namelist():
                needs_rewrite = True

        if needs_rewrite:
            import tempfile
            tmp_fd, tmp_path = tempfile.mkstemp(suffix='.tdvd', dir=os.path.dirname(tdvd_path) or '.')
            os.close(tmp_fd)
            with zipfile.ZipFile(tdvd_path, 'r') as zin, zipfile.ZipFile(tmp_path, 'w') as zout:
                for item in zin.infolist():
                    if item.filename != 'metadata.json':
                        zout.writestr(item, zin.read(item.filename))
                zout.writestr('metadata.json', data_bytes)
            os.replace(tmp_path, tdvd_path)
        else:
            with zipfile.ZipFile(tdvd_path, 'a') as zf:
                zf.writestr('metadata.json', data_bytes)
        print(f"[success] Embedded metadata.json into {tdvd_path}")
    
    if write_sidecar or not tdvd_path.endswith('.tdvd'):
        base = os.path.splitext(tdvd_path)[0]
        sidecar_path = f"{base}.json"
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
    cur_year = arg_year or existing_meta.get('year') or None

    print(f"Enriching: '{cur_title}' ({cur_year or 'unknown year'}) in {tdvd_path}...")

    disc_id = compute_dvd_disc_id(tdvd_path)
    if disc_id:
        print(f"  - Computed DVD Disc ID: {disc_id}")

    meta = None
    if arg_tmdb_key:
        print("  - Querying TMDb API...")
        meta = fetch_tmdb_metadata(cur_title, cur_year, arg_tmdb_key)

    if not meta:
        print("  - Querying Open Movie Database...")
        meta = fetch_open_metadata(cur_title, cur_year)

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

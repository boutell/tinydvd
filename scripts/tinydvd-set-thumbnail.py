#!/usr/bin/env python3
"""
tinydvd-set-thumbnail: Embed or update thumbnail images in .tdvd archives or directories.
Eliminates on-the-fly thumbnail generation in file managers like Nautilus.
"""

import sys
import os
import argparse
import subprocess
import zipfile
import tempfile
import glob

IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tif", ".tiff")
THUMBNAIL_CANDIDATES = ("thumbnail.png", "thumbnail.jpg", "cover.png", "cover.jpg", "poster.jpg", "poster.png")


def is_tdvd_archive(path):
    if not os.path.isfile(path):
        return False
    ext = os.path.splitext(path)[1].lower()
    if ext in (".tdvd", ".tinydvd", ".tvd"):
        return True
    return zipfile.is_zipfile(path)


def convert_image_to_png(input_path):
    """Convert an image (PNG, JPG, WEBP, etc.) to normalized PNG bytes."""
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
        tmp_path = tmp.name
    try:
        cmd = [
            "ffmpeg", "-y", "-loglevel", "error",
            "-i", os.path.abspath(input_path),
            "-vframes", "1",
            "-q:v", "2",
            tmp_path
        ]
        res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if res.returncode == 0 and os.path.exists(tmp_path) and os.path.getsize(tmp_path) > 0:
            with open(tmp_path, "rb") as f:
                return f.read()
    finally:
        if os.path.exists(tmp_path):
            try:
                os.unlink(tmp_path)
            except Exception:
                pass
    return None


def select_best_mkv(mkv_list_with_sizes):
    """
    Select best MKV to extract a thumbnail from.
    mkv_list_with_sizes: list of (filename, file_size)
    Prefers menu videos with content (> 100KB), then largest title video.
    """
    if not mkv_list_with_sizes:
        return None

    # Menu videos with content
    menus = [
        (name, size) for name, size in mkv_list_with_sizes
        if ("_0_video.mkv" in name.lower() or "video_ts_video.mkv" in name.lower()) and size > 100000
    ]
    if menus:
        # Sort menus by size descending
        menus.sort(key=lambda x: x[1], reverse=True)
        return menus[0][0]

    # Otherwise sort all MKVs by size descending (main movie)
    sorted_mkvs = sorted(mkv_list_with_sizes, key=lambda x: x[1], reverse=True)
    return sorted_mkvs[0][0]


def extract_frame_from_zip(zf, mkv_name, time_offset="00:00:02"):
    """Extract a representative frame from an MKV stored inside a zip file."""
    for t in [time_offset, "00:00:01", "00:00:00"]:
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
            tmp_path = tmp.name
        try:
            cmd = [
                "ffmpeg", "-y", "-loglevel", "error",
                "-i", "pipe:0",
                "-ss", t,
                "-vframes", "1",
                "-q:v", "2",
                tmp_path
            ]
            proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            with zf.open(mkv_name) as stream:
                try:
                    while True:
                        chunk = stream.read(65536)
                        if not chunk:
                            break
                        proc.stdin.write(chunk)
                except (BrokenPipeError, IOError):
                    pass
                finally:
                    try:
                        proc.stdin.close()
                    except Exception:
                        pass
            proc.wait()
            if os.path.exists(tmp_path) and os.path.getsize(tmp_path) > 0:
                with open(tmp_path, "rb") as f:
                    return f.read()
        finally:
            if os.path.exists(tmp_path):
                try:
                    os.unlink(tmp_path)
                except Exception:
                    pass
    return None


def extract_frame_from_dir(dir_path, mkv_name, time_offset="00:00:02"):
    """Extract a representative frame from an MKV file on disk."""
    mkv_path = os.path.join(dir_path, mkv_name)
    for t in [time_offset, "00:00:01", "00:00:00"]:
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
            tmp_path = tmp.name
        try:
            cmd = [
                "ffmpeg", "-y", "-loglevel", "error",
                "-ss", t,
                "-i", mkv_path,
                "-vframes", "1",
                "-q:v", "2",
                tmp_path
            ]
            res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if res.returncode == 0 and os.path.exists(tmp_path) and os.path.getsize(tmp_path) > 0:
                with open(tmp_path, "rb") as f:
                    return f.read()
        finally:
            if os.path.exists(tmp_path):
                try:
                    os.unlink(tmp_path)
                except Exception:
                    pass
    return None


def inspect_thumbnail(target):
    """Check if target already has an embedded thumbnail."""
    if os.path.isdir(target):
        for cand in THUMBNAIL_CANDIDATES:
            p = os.path.join(target, cand)
            if os.path.isfile(p) and os.path.getsize(p) > 0:
                return cand, os.path.getsize(p)
        return None, 0

    if is_tdvd_archive(target):
        try:
            with zipfile.ZipFile(target, "r") as zf:
                name_map = {n.lower(): n for n in zf.namelist()}
                for cand in THUMBNAIL_CANDIDATES:
                    real = name_map.get(cand)
                    if real:
                        info = zf.getinfo(real)
                        return real, info.file_size
        except Exception:
            return None, 0
    return None, 0


def process_target(target, custom_img_bytes=None, time_offset="00:00:02", force=False, verbose=False):
    """Add or update thumbnail in target (.tdvd file or directory)."""
    existing_name, existing_size = inspect_thumbnail(target)
    if existing_name and not force:
        return False, f"already has thumbnail '{existing_name}' ({existing_size // 1024} KB); use --force to overwrite"

    img_data = custom_img_bytes

    if os.path.isdir(target):
        if img_data is None:
            # Auto-extract from MKVs
            mkvs = []
            for f in os.listdir(target):
                if f.lower().endswith(".mkv"):
                    mkvs.append((f, os.path.getsize(os.path.join(target, f))))
            best = select_best_mkv(mkvs)
            if not best:
                return False, "no MKV videos found to extract thumbnail from"
            if verbose:
                print(f"  Extracting frame from {best} at {time_offset}...")
            img_data = extract_frame_from_dir(target, best, time_offset)
            if not img_data:
                return False, f"failed to extract frame from {best}"

        dest = os.path.join(target, "thumbnail.png")
        with open(dest, "wb") as f:
            f.write(img_data)
        return True, f"thumbnail.png created ({len(img_data) // 1024} KB)"

    elif is_tdvd_archive(target):
        if img_data is None:
            with zipfile.ZipFile(target, "r") as zf:
                mkvs = [
                    (info.filename, info.file_size)
                    for info in zf.infolist()
                    if info.filename.lower().endswith(".mkv")
                ]
                best = select_best_mkv(mkvs)
                if not best:
                    return False, "no MKV videos found in archive to extract thumbnail from"
                if verbose:
                    print(f"  Extracting frame from {best} at {time_offset}...")
                img_data = extract_frame_from_zip(zf, best, time_offset)
                if not img_data:
                    return False, f"failed to extract frame from {best}"

        # Embed thumbnail.png into ZIP archive uncompressed (ZIP_STORED)
        with zipfile.ZipFile(target, "a") as zf:
            # If overwriting, remove existing candidate entries from central directory list
            for cand in THUMBNAIL_CANDIDATES:
                matches = [info for info in zf.filelist if info.filename.lower() == cand]
                for m in matches:
                    zf.filelist.remove(m)
                    if m.filename in zf.NameToInfo:
                        del zf.NameToInfo[m.filename]

            zinfo = zipfile.ZipInfo(filename="thumbnail.png")
            zinfo.compress_type = zipfile.ZIP_STORED
            zinfo.file_size = len(img_data)
            zinfo.external_attr = 0o644 << 16
            zf.writestr(zinfo, img_data)

        return True, f"thumbnail.png embedded ({len(img_data) // 1024} KB)"

    else:
        return False, "not a valid .tdvd archive or directory"


def main():
    parser = argparse.ArgumentParser(
        description="Add, embed, or update thumbnail images in .tdvd archives or directories."
    )
    parser.add_argument(
        "targets",
        nargs="+",
        help="One or more .tdvd files, directories, or glob patterns (e.g. *.tdvd)"
    )
    parser.add_argument(
        "-i", "--image",
        help="Path to custom image file (PNG, JPG, WEBP, etc.). If omitted, extracts frame from MKV."
    )
    parser.add_argument(
        "-t", "--time",
        default="00:00:02",
        help="Video timestamp offset for frame extraction (default: 00:00:02)"
    )
    parser.add_argument(
        "-f", "--force",
        action="store_true",
        help="Overwrite existing thumbnail if present"
    )
    parser.add_argument(
        "-c", "--check", "--list",
        action="store_true",
        dest="check_mode",
        help="List thumbnail status of targets without modifying them"
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose logging"
    )

    args = parser.parse_args()

    # Detect shorthand: tinydvd-set-thumbnail movie.tdvd custom_cover.jpg
    targets = list(args.targets)
    custom_img_path = args.image

    if custom_img_path is None and len(targets) == 2:
        if (is_tdvd_archive(targets[0]) or os.path.isdir(targets[0])) and os.path.isfile(targets[1]):
            ext = os.path.splitext(targets[1])[1].lower()
            if ext in IMAGE_EXTENSIONS:
                custom_img_path = targets[1]
                targets = [targets[0]]

    # Expand any glob patterns
    expanded_targets = []
    for t in targets:
        matches = glob.glob(t)
        if matches:
            expanded_targets.extend(matches)
        else:
            expanded_targets.append(t)

    # Filter unique targets
    targets = []
    seen = set()
    for t in expanded_targets:
        norm = os.path.abspath(t)
        if norm not in seen:
            seen.add(norm)
            targets.append(t)

    if not targets:
        print("No valid target files or directories specified.", file=sys.stderr)
        sys.exit(1)

    # Check mode
    if args.check_mode:
        print(f"Checking {len(targets)} target(s):")
        missing_count = 0
        for t in targets:
            name, size = inspect_thumbnail(t)
            base = os.path.basename(t.rstrip("/\\")) or t
            if name:
                print(f"  [PRESENT] {base}: {name} ({size // 1024} KB)")
            else:
                print(f"  [MISSING] {base}: no embedded thumbnail")
                missing_count += 1
        if missing_count > 0:
            print(f"\n{missing_count} package(s) missing thumbnails.")
            sys.exit(1)
        else:
            print("\nAll packages have embedded thumbnails.")
            sys.exit(0)

    # Prepare custom image bytes if specified
    custom_bytes = None
    if custom_img_path:
        if not os.path.isfile(custom_img_path):
            print(f"Error: image file '{custom_img_path}' not found.", file=sys.stderr)
            sys.exit(1)
        if args.verbose:
            print(f"Converting image '{custom_img_path}' to PNG...")
        custom_bytes = convert_image_to_png(custom_img_path)
        if not custom_bytes:
            print(f"Error: failed to process image '{custom_img_path}'.", file=sys.stderr)
            sys.exit(1)

    # Process all targets
    success_count = 0
    fail_count = 0
    skip_count = 0

    for t in targets:
        base = os.path.basename(t.rstrip("/\\")) or t
        ok, msg = process_target(
            t,
            custom_img_bytes=custom_bytes,
            time_offset=args.time,
            force=args.force,
            verbose=args.verbose
        )
        if ok:
            print(f"✓ {base}: {msg}")
            success_count += 1
        elif "already has" in msg:
            print(f"- {base}: {msg}")
            skip_count += 1
        else:
            print(f"✗ {base}: {msg}", file=sys.stderr)
            fail_count += 1

    print(f"\nDone: {success_count} updated, {skip_count} skipped, {fail_count} failed.")
    sys.exit(1 if fail_count > 0 else 0)


if __name__ == "__main__":
    main()

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


def thumbnailer_path():
    """The tinydvd thumbnailer next to this script, which picks the frame."""
    here = os.path.dirname(os.path.realpath(__file__))
    for name in ("tinydvd-thumbnailer.py", "tinydvd-thumbnailer"):
        p = os.path.join(here, name)
        if os.path.isfile(p):
            return p
    return None


def extract_frame(target, time_offset=None, verbose=False):
    """
    Pick a frame with the thumbnailer, ignoring any embedded thumbnail, at
    full size. Returns PNG bytes, or None.
    """
    thumbnailer = thumbnailer_path()
    if not thumbnailer:
        print("Error: tinydvd-thumbnailer not found next to this script.", file=sys.stderr)
        return None
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
        tmp_path = tmp.name
    try:
        cmd = [sys.executable, thumbnailer, "--frame-only", "-s", "0"]
        if time_offset is not None:
            cmd += ["-t", str(time_offset)]
        if verbose:
            cmd.append("-v")
        cmd += [os.path.abspath(target), tmp_path]
        res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=None if verbose else subprocess.DEVNULL)
        if res.returncode == 0 and os.path.getsize(tmp_path) > 0:
            with open(tmp_path, "rb") as f:
                return f.read()
        return None
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)


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


def process_target(target, custom_img_bytes=None, time_offset=None, force=False, verbose=False):
    """Add or update thumbnail in target (.tdvd file or directory)."""
    existing_name, existing_size = inspect_thumbnail(target)
    if existing_name and not force:
        return False, f"already has thumbnail '{existing_name}' ({existing_size // 1024} KB); use --force to overwrite"

    img_data = custom_img_bytes

    if os.path.isdir(target):
        if img_data is None:
            img_data = extract_frame(target, time_offset, verbose)
            if not img_data:
                return False, "couldn't extract a frame from its videos"

        dest = os.path.join(target, "thumbnail.png")
        with open(dest, "wb") as f:
            f.write(img_data)
        return True, f"thumbnail.png created ({len(img_data) // 1024} KB)"

    elif is_tdvd_archive(target):
        if img_data is None:
            img_data = extract_frame(target, time_offset, verbose)
            if not img_data:
                return False, "couldn't extract a frame from its videos"

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
        help="Path to custom image file (PNG, JPG, WEBP, etc.). If omitted, a frame is picked from the videos."
    )
    parser.add_argument(
        "-t", "--time",
        help="Take the frame this far into the main feature (seconds or HH:MM:SS). "
             "By default the disc's main menu is used, or 30 seconds into the main feature."
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

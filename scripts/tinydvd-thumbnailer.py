#!/usr/bin/env python3
"""
tinydvd-thumbnailer: FreeDesktop/GNOME thumbnailer for .tdvd packages.

Uses the package's embedded thumbnail.png / cover image when there is one.
Otherwise it picks a frame from the disc's videos:

1. The menu video of the title set that holds the main feature
   (VTS_NN_0_VIDEO.mkv, where VTS_NN_VIDEO.mkv is the largest title video).
   That is usually the disc's main menu, which makes the best thumbnail.
   The disc-level menu video (VIDEO_TS_VIDEO.mkv) is skipped: it usually
   holds the copyright and region warnings that play before the menu.
2. Otherwise, 30 seconds into the largest video, past any fade-in.

tinydvd-set-thumbnail uses this script (with --frame-only) to choose the
frame it embeds, so both always agree.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

EMBEDDED_CANDIDATES = ["thumbnail.png", "thumbnail.jpg", "cover.png", "cover.jpg", "poster.jpg", "poster.png"]
TITLE_VIDEO_RE = re.compile(r"^VTS_(\d\d)_VIDEO\.mkv$", re.IGNORECASE)
MIN_MENU_BYTES = 100 * 1024
MENU_OFFSET = 5          # seconds into the menu video, once any fade-in is over
FEATURE_OFFSET = 30      # seconds into the largest video, past any fade-in


def extract_embedded_image(path):
    """Return the bytes of an embedded thumbnail / cover image, or None."""
    if os.path.isdir(path):
        for name in EMBEDDED_CANDIDATES:
            p = os.path.join(path, name)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    return f.read()
        return None
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path, "r") as zf:
            name_map = {n.lower(): n for n in zf.namelist()}
            for cand in EMBEDDED_CANDIDATES:
                real_name = name_map.get(cand)
                if real_name:
                    return zf.read(real_name)
    return None


def list_videos(path):
    """
    Return {basename: (size, source)} for the package's .mkv videos, where
    source is either a URL ffmpeg can open and seek in directly, or
    ("zip", name) for a compressed archive entry that has to be streamed.
    """
    videos = {}
    if os.path.isdir(path):
        for name in os.listdir(path):
            if name.lower().endswith(".mkv"):
                full = os.path.join(path, name)
                videos[name] = (os.path.getsize(full), "file:" + os.path.abspath(full))
        return videos
    if not zipfile.is_zipfile(path):
        return videos
    with zipfile.ZipFile(path, "r") as zf, open(path, "rb") as raw:
        for info in zf.infolist():
            name = os.path.basename(info.filename)
            if not name.lower().endswith(".mkv"):
                continue
            if info.compress_type == zipfile.ZIP_STORED:
                # .tdvd stores videos uncompressed, so ffmpeg can read the
                # entry's bytes in place and seek without streaming the file
                raw.seek(info.header_offset)
                header = raw.read(30)
                name_len, extra_len = struct.unpack("<HH", header[26:30])
                start = info.header_offset + 30 + name_len + extra_len
                end = start + info.file_size
                source = f"subfile,,start,{start},end,{end},,:file:{os.path.abspath(path)}"
            else:
                source = ("zip", info.filename)
            videos[name] = (info.file_size, source)
    return videos


def frame_candidates(videos, time_offset=None):
    """Return [(video name, seconds)] to try, best first."""
    if not videos:
        return []
    titles = [n for n in videos if TITLE_VIDEO_RE.match(n)]
    largest = max(titles or videos, key=lambda n: videos[n][0])
    if time_offset is not None:
        return [(largest, time_offset), (largest, 0)]
    candidates = []
    m = TITLE_VIDEO_RE.match(largest)
    if m:
        menu = next((n for n in videos if n.lower() == f"vts_{m.group(1)}_0_video.mkv"), None)
        if menu and videos[menu][0] >= MIN_MENU_BYTES:
            candidates += [(menu, MENU_OFFSET), (menu, 0)]
    candidates += [(largest, FEATURE_OFFSET), (largest, 0)]
    return candidates


def grab_frame(package, source, seconds, out_path, max_size):
    """Write one frame at `seconds` to out_path as PNG. Returns True on success."""
    vf = []
    if max_size:
        vf = ["-vf", f"scale='min({max_size},iw)':'min({max_size},ih)':force_original_aspect_ratio=decrease"]
    out = ["-frames:v", "1", "-f", "image2", "-c:v", "png", out_path]
    if isinstance(source, str):
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-ss", str(seconds), "-i", source] + vf + out
        ok = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    else:
        # Compressed archive entry: stream it, decoding up to `seconds`
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", "pipe:0", "-ss", str(seconds)] + vf + out
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with zipfile.ZipFile(package, "r") as zf, zf.open(source[1]) as stream:
            try:
                while True:
                    chunk = stream.read(65536)
                    if not chunk:
                        break
                    proc.stdin.write(chunk)
            except (BrokenPipeError, OSError):
                pass
            finally:
                try:
                    proc.stdin.close()
                except OSError:
                    pass
        ok = proc.wait() == 0
    return ok and os.path.exists(out_path) and os.path.getsize(out_path) > 0


def pick_frame(package, out_path, max_size=0, time_offset=None, verbose=False):
    """Write the best frame of the package to out_path. Returns (video, seconds) or None."""
    videos = list_videos(package)
    for name, seconds in frame_candidates(videos, time_offset):
        if verbose:
            print(f"  Trying {name} at {seconds}s...", file=sys.stderr)
        if grab_frame(package, videos[name][1], seconds, out_path, max_size):
            return name, seconds
    return None


def write_image(img_bytes, out_path, max_size):
    """Write an embedded image to out_path as a PNG scaled to max_size."""
    with tempfile.NamedTemporaryFile(suffix=".img", delete=False) as tmp:
        tmp.write(img_bytes)
        tmp_path = tmp.name
    try:
        return grab_frame(None, "file:" + tmp_path, 0, out_path, max_size)
    finally:
        os.unlink(tmp_path)


def main():
    parser = argparse.ArgumentParser(description="tinydvd FreeDesktop/GNOME thumbnailer")
    parser.add_argument("-s", "--size", type=int, default=256,
                        help="Maximum thumbnail dimension (default: 256, 0 for full size)")
    parser.add_argument("--frame-only", action="store_true",
                        help="Ignore any embedded thumbnail and pick a video frame")
    parser.add_argument("-t", "--time",
                        help="Take the frame this far into the largest video instead (seconds or HH:MM:SS)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Say which video frames are tried")
    parser.add_argument("input", help="Input .tdvd / .tinydvd file or directory")
    parser.add_argument("output", help="Output PNG file path")
    args = parser.parse_args()

    inp = os.path.abspath(args.input)
    out = os.path.abspath(args.output)
    max_size = 0 if args.size == 0 else max(64, min(args.size, 1024))

    if not args.frame_only and args.time is None:
        img = extract_embedded_image(inp)
        if img and write_image(img, out, max_size):
            sys.exit(0)

    choice = pick_frame(inp, out, max_size, args.time, args.verbose)
    if choice:
        if args.verbose:
            print(f"  Used {choice[0]} at {choice[1]}s", file=sys.stderr)
        sys.exit(0)
    sys.exit(1)


if __name__ == "__main__":
    main()

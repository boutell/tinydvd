#!/usr/bin/env python3
"""
tinydvd-thumbnailer: FreeDesktop/GNOME thumbnailer for .tdvd packages.

Uses the package's embedded thumbnail.png when there is one.
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

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile

import tinydvd_archive

TITLE_VIDEO_RE = re.compile(r"^VTS_(\d\d)_VIDEO\.mkv$", re.IGNORECASE)
MIN_MENU_BYTES = 100 * 1024
MENU_OFFSET = 5          # seconds into the menu video, once any fade-in is over
FEATURE_OFFSET = 30      # seconds into the largest video, past any fade-in


def list_videos(path):
    """{basename: (size, MemberLocation)} for the package's .mkv videos."""
    videos = {}
    for name, size in tinydvd_archive.list_members(path).items():
        if name.lower().endswith(".mkv"):
            loc = tinydvd_archive.locate_member(path, name)
            if loc:
                videos[name] = (size, loc)
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


def grab_frame(source, seconds, out_path, max_size):
    """
    Write one frame at `seconds` to out_path as PNG. source is an ffmpeg input
    URL, or a MemberLocation to pipe in. Returns True on success.
    """
    # Unsqueeze anamorphic DVD frames (e.g. 720x480 SAR 32:27 -> 854x480 SAR 1:1) into square pixels
    unsquish = "scale='round(iw*max(1,if(gt(sar,0),sar,1))/2)*2':'round(ih/min(1,if(gt(sar,0),sar,1))/2)*2',setsar=1"
    if max_size:
        vf = ["-vf", f"{unsquish},scale='min({max_size},iw)':'min({max_size},ih)':force_original_aspect_ratio=decrease,setsar=1"]
    else:
        vf = ["-vf", unsquish]
    out = ["-frames:v", "1", "-f", "image2", "-c:v", "png", out_path]
    url = source if isinstance(source, str) else source.ffmpeg_input()
    if url:
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-ss", str(seconds), "-i", url] + vf + out
        ok = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    else:
        # Compressed archive entry: stream it, decoding up to `seconds`
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", "pipe:0", "-ss", str(seconds)] + vf + out
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            source.copy_to(proc.stdin)
        except OSError:
            pass  # ffmpeg stops reading once it has its frame
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
        if grab_frame(videos[name][1], seconds, out_path, max_size):
            return name, seconds
    return None


def write_image(img_bytes, out_path, max_size):
    """Write an embedded image to out_path as a PNG scaled to max_size."""
    with tempfile.NamedTemporaryFile(suffix=".img", delete=False) as tmp:
        tmp.write(img_bytes)
        tmp_path = tmp.name
    try:
        return grab_frame("file:" + tmp_path, 0, out_path, max_size)
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
        img = tinydvd_archive.read_thumbnail(inp)
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

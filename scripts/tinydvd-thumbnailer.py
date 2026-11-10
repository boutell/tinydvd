#!/usr/bin/env python3
"""
tinydvd-thumbnailer: FreeDesktop/GNOME thumbnailer for .tdvd packages.
Extracts embedded thumbnail.png / cover.jpg, or generates a frame from companion MKVs.
"""

import sys
import os
import argparse
import subprocess
import zipfile
import tempfile


def extract_embedded_image(zip_path_or_dir, candidate_names):
    """Try to find and return bytes of an embedded image file."""
    if os.path.isdir(zip_path_or_dir):
        for name in candidate_names:
            p = os.path.join(zip_path_or_dir, name)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    return f.read()
        return None

    if zipfile.is_zipfile(zip_path_or_dir):
        with zipfile.ZipFile(zip_path_or_dir, "r") as zf:
            name_map = {n.lower(): n for n in zf.namelist()}
            for cand in candidate_names:
                real_name = name_map.get(cand.lower())
                if real_name:
                    return zf.read(real_name)
    return None


def resize_image_ffmpeg(img_bytes, out_path, max_size):
    """Write image to out_path, optionally scaled to max_size using ffmpeg."""
    scale_filter = f"scale='min({max_size},iw)':'min({max_size},ih)':force_original_aspect_ratio=decrease"
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-i", "pipe:0",
        "-vf", scale_filter,
        "-vframes", "1",
        out_path
    ]
    try:
        proc = subprocess.run(cmd, input=img_bytes, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return proc.returncode == 0
    except Exception:
        # Fallback: direct write
        try:
            with open(out_path, "wb") as f:
                f.write(img_bytes)
            return True
        except Exception:
            return False


def extract_frame_from_mkv(zip_path_or_dir, out_path, max_size):
    """Extract a representative frame from the best MKV in the package."""
    scale_filter = f"scale='min({max_size},iw)':'min({max_size},ih)':force_original_aspect_ratio=decrease"

    if os.path.isdir(zip_path_or_dir):
        # Find best MKV (menu or title)
        files = os.listdir(zip_path_or_dir)
        mkvs = [f for f in files if f.lower().endswith(".mkv")]
        if not mkvs:
            return False
        # Sort so menus with content come first, then largest title
        menus = [m for m in mkvs if "_0_video.mkv" in m.lower() or "video_ts_video.mkv" in m.lower()]
        best_mkv = os.path.join(zip_path_or_dir, menus[0] if menus else mkvs[0])
        cmd = [
            "ffmpeg", "-y", "-loglevel", "error",
            "-ss", "00:00:02",
            "-i", best_mkv,
            "-vf", scale_filter,
            "-vframes", "1",
            out_path
        ]
        return subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0

    if zipfile.is_zipfile(zip_path_or_dir):
        with zipfile.ZipFile(zip_path_or_dir, "r") as zf:
            mkvs = [n for n in zf.namelist() if n.lower().endswith(".mkv")]
            if not mkvs:
                return False
            # Prefer menu video > 1MB, or first MKV
            menus = [m for m in mkvs if "_0_video.mkv" in m.lower() or "video_ts_video.mkv" in m.lower()]
            target_mkv = menus[0] if menus else mkvs[0]

            for t in ["00:00:02", "00:00:00"]:
                cmd = [
                    "ffmpeg", "-y", "-loglevel", "error",
                    "-i", "pipe:0",
                    "-ss", t,
                    "-vf", scale_filter,
                    "-vframes", "1",
                    out_path
                ]
                proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                with zf.open(target_mkv) as stream:
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
                if proc.returncode == 0 and os.path.exists(out_path) and os.path.getsize(out_path) > 0:
                    return True
            return False

    return False


def main():
    parser = argparse.ArgumentParser(description="tinydvd FreeDesktop/GNOME thumbnailer")
    parser.add_argument("-s", "--size", type=int, default=256, help="Maximum thumbnail dimension (default: 256)")
    parser.add_argument("input", help="Input .tdvd / .tinydvd file or directory")
    parser.add_argument("output", help="Output PNG file path")

    args = parser.parse_args()

    inp = os.path.abspath(args.input)
    out = os.path.abspath(args.output)
    max_size = max(64, min(args.size, 1024))

    # 1. Try embedded thumbnail / cover image
    candidates = ["thumbnail.png", "thumbnail.jpg", "cover.png", "cover.jpg", "poster.jpg", "poster.png"]
    img_data = extract_embedded_image(inp, candidates)
    if img_data:
        if resize_image_ffmpeg(img_data, out, max_size):
            sys.exit(0)

    # 2. Extract from companion MKV
    if extract_frame_from_mkv(inp, out, max_size):
        sys.exit(0)

    sys.exit(1)


if __name__ == "__main__":
    main()

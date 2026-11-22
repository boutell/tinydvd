"""
Measuring audio/video sync on the test disc's title, which flashes white
for 3 frames at 3.003 s while audio track 1 beeps at 1 kHz and track 2 at
500 Hz (see tests/fixtures/make-test-disc.sh).

Times come from decoding with ffmpeg, which honours edit lists the way the
players do.
"""

from __future__ import annotations

import subprocess

FLASH_AT = 3.003          # when the flash and beeps start on the test disc
SAMPLE_RATE = 48000


def _run(cmd: list[str]) -> bytes:
    return subprocess.run(cmd, capture_output=True, check=True, stdin=subprocess.DEVNULL).stdout


def flash_time(src: str, stream: str = "v:0") -> float | None:
    """When the first mostly-white frame is shown, in seconds."""
    times = [float(t.strip(",")) for t in _run(
        ["ffprobe", "-v", "error", "-select_streams", stream, "-show_entries", "frame=pts_time",
         "-of", "csv=p=0", src]).decode().split() if t.strip(",") not in ("", "N/A")]
    w, h = 16, 12
    raw = _run(["ffmpeg", "-v", "error", "-nostdin", "-i", src, "-map", f"0:{stream}", "-fps_mode", "passthrough",
                "-vf", f"scale={w}:{h},format=gray", "-f", "rawvideo", "-"])
    size = w * h
    for i in range(min(len(times), len(raw) // size)):
        frame = raw[i * size:(i + 1) * size]
        if sum(frame) / size > 200:
            return times[i]
    return None


def beep(src: str, stream: str = "a:0") -> tuple[float, float] | None:
    """(when the first beep starts, its pitch in Hz), on the file's timeline."""
    pcm = _run(["ffmpeg", "-v", "error", "-nostdin", "-copyts", "-i", src, "-map", f"0:{stream}",
                "-af", "aresample=async=1:first_pts=0", "-ac", "1", "-ar", str(SAMPLE_RATE),
                "-f", "s16le", "-"])
    samples = memoryview(pcm).cast("h")
    onset = next((i for i, s in enumerate(samples) if abs(s) > 3000), None)
    if onset is None:
        return None
    window = samples[onset:onset + SAMPLE_RATE // 20]  # 50 ms
    crossings = sum(1 for a, b in zip(window, window[1:]) if (a < 0) != (b < 0))
    return onset / SAMPLE_RATE, crossings / 2 / (len(window) / SAMPLE_RATE)

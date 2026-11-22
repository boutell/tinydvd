#!/usr/bin/env python3
"""
Audio/video sync and audio track order, on the test disc's title: it flashes
white at 3.003 s while audio track 1 beeps at 1 kHz and track 2 at 500 Hz
(see tests/fixtures/make-test-disc.sh). The converter stores the video and
each audio track as separate files; these check they still line up:

- in the package's files, as ffmpeg decodes them (it honours their edit
  lists, as the players do),
- in what mpv actually plays, with the audio files as tracks, recorded with
  mpv's encode mode, and
- through the converter's encode for full-length titles. The test disc's
  title is short, so the converter encodes it segment by segment, as it does
  menus; this builds a DVD-style MPEG with the same flash and beeps (with
  ffmpeg alone) and runs it through the title encode as well.

Lip sync errors start to be noticeable at a few tens of milliseconds; the
converter's start-up delays (a frame or two of video, 21 ms of AAC priming)
are that big, so a few milliseconds' tolerance catches any of them.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import importlib
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import av_sync  # noqa: E402

FIXTURE = os.path.join(HERE, "fixtures", "TestDisc.tdvd")
TOLERANCE = 0.003
PITCHES = {1: 1000, 2: 500}


def check(label: str, flash: float | None, beep: tuple[float, float] | None, track: int) -> None:
    assert flash is not None, f"{label}: no flash found"
    assert beep is not None, f"{label}: no beep found"
    start, pitch = beep
    assert abs(flash - av_sync.FLASH_AT) < 0.002, f"{label}: flash at {flash:.4f}, expected {av_sync.FLASH_AT}"
    assert abs(start - flash) <= TOLERANCE, \
        f"{label}: audio {1000 * (start - flash):+.1f} ms from the picture (allowed {1000 * TOLERANCE:.0f})"
    assert abs(pitch - PITCHES[track]) < 50, f"{label}: expected track {track}'s {PITCHES[track]} Hz beep, heard {pitch:.0f} Hz"
    print(f"  - {label}: audio {1000 * (start - flash):+.1f} ms from the picture, {pitch:.0f} Hz: OK")


def main() -> int:
    print("=== Audio/video sync tests ===")
    with tempfile.TemporaryDirectory() as d:
        with zipfile.ZipFile(FIXTURE) as zf:
            for name in ("VTS_01_VIDEO.mp4", "VTS_01_AUDIO_1.mp4", "VTS_01_AUDIO_2.mp4"):
                zf.extract(name, d)
        video = os.path.join(d, "VTS_01_VIDEO.mp4")
        audio = [os.path.join(d, f"VTS_01_AUDIO_{n}.mp4") for n in (1, 2)]

        print("In the package's files:")
        flash = av_sync.flash_time(video)
        for n in (1, 2):
            check(f"track {n}", flash, av_sync.beep(audio[n - 1]), n)

        print("Through the converter's full-length title encode:")
        title = os.path.join(d, "title.mpg")
        flash = "between(t,3.003,3.103)"
        subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-y",
                        "-f", "lavfi", "-i", f"testsrc=s=720x480:r=30000/1001:d=6,drawbox=c=white:t=fill:enable='{flash}'",
                        "-f", "lavfi", "-i", f"aevalsrc='if({flash},0.5*sin(2*PI*1000*t),0)':s=48000:c=stereo:d=6",
                        "-f", "lavfi", "-i", f"aevalsrc='if({flash},0.5*sin(2*PI*500*t),0)':s=48000:c=stereo:d=6",
                        "-map", "0:v", "-map", "1:a", "-map", "2:a", "-t", "6",
                        "-target", "ntsc-dvd", "-aspect", "4:3", "-ac", "2", title], check=True)
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), "scripts"))
        converter = importlib.import_module("convert-to-tinydvd")
        out = os.path.join(d, "title")
        os.makedirs(out)
        work = converter.work_path(out, "VTS_01_VIDEO.mp4")
        converter.transcode_video([title], None, work, crf=30, preset="veryfast")
        converter.finish_video(work, out, "VTS_01_VIDEO.mp4")
        t_flash = av_sync.flash_time(os.path.join(out, "VTS_01_VIDEO.mp4"))
        for n in (1, 2):
            check(f"title encode, track {n}", t_flash, av_sync.beep(os.path.join(out, f"VTS_01_AUDIO_{n}.mp4")), n)

        print("Through the segment-by-segment encode, with several segments:")
        # Menus are encoded one segment (VOB_ID) at a time and joined. Two
        # 2-second DVD-style segments; the second flashes and beeps 1.001 s
        # in (frame 30), so 3.001 s into the joined video
        segments = []
        for k, flash_at in enumerate((None, 1.001)):
            mpg = os.path.join(d, f"seg{k}.mpg")
            enable = f"between(t,{flash_at},{flash_at + 0.1})" if flash_at else "0"
            subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-y",
                            "-f", "lavfi", "-i", f"testsrc=s=720x480:r=30000/1001:d=2,drawbox=c=white:t=fill:enable='{enable}'",
                            "-f", "lavfi", "-i", f"aevalsrc='if({enable},0.5*sin(2*PI*1000*t),0)':s=48000:c=stereo:d=2",
                            "-map", "0:v", "-map", "1:a", "-t", "2", "-target", "ntsc-dvd", "-aspect", "4:3", "-ac", "2", mpg],
                           check=True)
            segments.append({"vob_path": mpg, "start_byte": 0, "end_byte": os.path.getsize(mpg),
                             "duration_sec": 2.0, "audio_sectors": 1, "vob_id": k + 1})
        menu = os.path.join(d, "menu")
        os.makedirs(menu)
        work = converter.work_path(menu, "VTS_01_0_VIDEO.mp4")
        converter._transcode_segmented_vob(segments, work, crf=30, preset="veryfast", audio_ids=[0x80])
        converter.finish_video(work, menu, "VTS_01_0_VIDEO.mp4")
        m_flash = av_sync.flash_time(os.path.join(menu, "VTS_01_0_VIDEO.mp4"))
        m_beep = av_sync.beep(os.path.join(menu, "VTS_01_0_AUDIO_1.mp4"))
        # The second segment must start exactly 2 s in, where the disc expects it
        assert m_flash is not None and abs(m_flash - 3.001) < 0.002, f"segments: flash at {m_flash}, expected 3.001"
        assert m_beep is not None and abs(m_beep[0] - m_flash) <= TOLERANCE, \
            f"segments: audio {1000 * (m_beep[0] - m_flash):+.1f} ms from the picture"
        print(f"  - second segment in place (flash at {m_flash:.3f}s), "
              f"audio {1000 * (m_beep[0] - m_flash):+.1f} ms from the picture: OK")

        mpv = shutil.which("mpv")
        if not mpv:
            print("Skipping mpv playback sync (no mpv command)")
        else:
            print("As mpv plays them:")
            for n in (1, 2):
                out = os.path.join(d, f"played_{n}.mkv")
                subprocess.run([mpv, "--no-config", "--really-quiet", f"--audio-files={audio[0]}",
                                f"--audio-files-append={audio[1]}", f"--aid={n}", "--length=5",
                                f"--o={out}", "--of=matroska", "--ovc=rawvideo", "--oac=pcm_s16le", video],
                               check=True, stdin=subprocess.DEVNULL)
                check(f"mpv, track {n}", av_sync.flash_time(out), av_sync.beep(out), n)
    print("All audio/video sync tests passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())

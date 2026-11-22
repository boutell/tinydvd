"""
The video files in a TinyDVD package, and reading them for the web player.

Each DVD video (a title set's titles, its menus, or the disc menu) is stored
as one fragmented MP4 of the video plus one per audio track:

    VTS_04_VIDEO.mp4      H.264 video only
    VTS_04_AUDIO_1.mp4    AAC, the DVD's audio stream 1
    VTS_04_AUDIO_2.mp4    ...

The same files serve both players, so nothing is stored twice:
- mpv (the desktop player) plays the video file and adds the audio files.
- The web player streams them as HLS: playlists of byte ranges, read from
  the index (sidx) at the start of each file, served straight out of the
  package. Separate audio files mean a browser downloads only the track
  being heard.

Fragmented MP4 can't hide the encoder's start-up delays the way a plain
MP4 does: ffmpeg would start the video a frame or two late (frame
reordering) and the audio 21 ms late (AAC priming). split_video measures
each and adds an edit list (elst) saying so. mpv (via FFmpeg) and hls.js
honour them (tests/test_av_sync.py and test_mpv_playback.c check), so the
players see the original timeline exactly: frames at their times, and the
sound in sync with the picture.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import struct
import subprocess

VIDEO_MEMBER_RE = re.compile(r"^(?P<base>.+)_VIDEO\.mp4$", re.IGNORECASE)
AUDIO_MEMBER_RE = re.compile(r"^(?P<base>.+)_AUDIO_(?P<n>\d+)\.mp4$", re.IGNORECASE)

# Video fragments start at each keyframe (the converter puts one about every
# second) once 2 s have passed, so seeking costs little. Audio fragments are
# longer: every fragment has a small header, and 6 s ones keep that overhead
# to about 0.1% of the audio
VIDEO_MUX = ["-movflags", "+frag_keyframe+empty_moov+default_base_moof+global_sidx+skip_trailer",
             "-frag_duration", "2000000"]
AUDIO_MUX = ["-movflags", "+empty_moov+default_base_moof+global_sidx+skip_trailer",
             "-frag_duration", "6000000"]

COPY_CHUNK = 4 * 1024 * 1024


def audio_member_name(video_name: str, n: int) -> str:
    """'VTS_04_VIDEO.mp4', 1 -> 'VTS_04_AUDIO_1.mp4'."""
    m = VIDEO_MEMBER_RE.match(video_name)
    if not m:
        raise ValueError(f"not a video member name: {video_name}")
    return f"{m.group('base')}_AUDIO_{n}.mp4"


def audio_member_names(video_name: str, members) -> list[str]:
    """The audio files that go with a video, in track order (1, 2, ...), from a list of names."""
    lower = {name.lower(): name for name in members}
    out = []
    n = 1
    while True:
        name = lower.get(audio_member_name(video_name, n).lower())
        if not name:
            return out
        out.append(name)
        n += 1


# -- Probing --------------------------------------------------------------


def _ffprobe_lines(src: str, args: list[str]) -> list[str]:
    try:
        out = subprocess.run(["ffprobe", "-v", "error", *args, "-of", "csv=p=0", src],
                             capture_output=True, text=True, timeout=120,
                             stdin=subprocess.DEVNULL).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return [line.strip().strip(",") for line in out.split() if line.strip()]


def first_frame_pts(src: str, stream: str = "v:0") -> float | None:
    """When a stream's first decoded frame is presented, in seconds."""
    times = []
    for line in _ffprobe_lines(src, ["-select_streams", stream, "-show_entries", "frame=pts_time",
                                     "-read_intervals", "%+#8"]):
        try:
            times.append(float(line))
        except ValueError:
            pass
    return min(times) if times else None


def audio_start(src: str, stream: str) -> tuple[float, float] | None:
    """
    An audio stream's first packet time, and how much encoder priming (in
    seconds) the file says to skip there. The real sound starts at their
    sum, whether or not a file has that note: without it, the priming is
    still in the stream, it just gets played as silence first.
    """
    try:
        out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", stream, "-show_entries",
                              "packet=pts_time:packet_side_data=skip_samples:stream=sample_rate",
                              "-read_intervals", "%+#1", "-of", "json", src],
                             capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL).stdout
        info = json.loads(out or "{}")
    except (OSError, subprocess.SubprocessError, ValueError):
        return None
    packets = info.get("packets") or []
    if not packets or "pts_time" not in packets[0]:
        return None
    pts = float(packets[0]["pts_time"])
    skip = sum(int(d.get("skip_samples", 0)) for d in packets[0].get("side_data_list", []))
    rate = int((info.get("streams") or [{}])[0].get("sample_rate", 0) or 0)
    return pts, (skip / rate if skip and rate else 0.0)


def audio_stream_count(src: str) -> int:
    return len(_ffprobe_lines(src, ["-select_streams", "a", "-show_entries", "stream=index"]))


# -- Writing --------------------------------------------------------------


def split_video(src: str, out_dir: str, video_name: str, log=print) -> list[str]:
    """
    Write video_name (e.g. 'VTS_04_VIDEO.mp4') and its audio files into
    out_dir from src, a muxed video (an MKV) as an ffmpeg input: a path or
    a subfile: URL. No re-encoding. Returns the names written.
    """
    if not VIDEO_MEMBER_RE.match(video_name):
        raise ValueError(f"not a video member name: {video_name}")
    n_audio = audio_stream_count(src)
    video_path = os.path.join(out_dir, video_name)
    names = [video_name]
    cmd = ["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error", "-i", src,
           "-map", "0:v:0", "-c", "copy", *VIDEO_MUX, "-f", "mp4", video_path]
    for i in range(n_audio):
        name = audio_member_name(video_name, i + 1)
        names.append(name)
        cmd += ["-map", f"0:a:{i}", "-c", "copy", *AUDIO_MUX, "-f", "mp4", os.path.join(out_dir, name)]
    res = subprocess.run(cmd, stdin=subprocess.DEVNULL)
    if res.returncode != 0:
        raise RuntimeError(f"ffmpeg couldn't split {video_name} (exit code {res.returncode})")

    # Put every track where it belongs (see the module docstring). The
    # disc's timeline starts at the first video frame, so the video's first
    # frame goes at exactly 0; each audio track keeps its place relative to
    # the video in src. The muxer may have moved either: the video by its
    # frame reordering delay, the audio by dropping src's note to skip the
    # AAC encoder's priming samples (which it then plays, late, as sound).
    # Both come out late, never early, so an edit list that skips the
    # difference at the start puts each right
    video_src = first_frame_pts(src, "v:0")
    corrections = {}
    if video_src is not None:
        out_first = first_frame_pts(video_path, "v:0")
        if out_first is not None and out_first > 0.0005:
            corrections[video_name] = out_first
        for i, name in enumerate(names[1:]):
            src_audio = audio_start(src, f"a:{i}")
            out_audio = audio_start(os.path.join(out_dir, name), "a:0")
            if src_audio is None or out_audio is None:
                continue
            priming = src_audio[1]
            sound_src = src_audio[0] + priming
            sound_out = out_audio[0] + priming
            skip = sound_out - (sound_src - video_src)
            if skip > 0.0005:
                corrections[name] = skip
            elif skip < -0.0005:
                log(f"[mp4] Warning: {name} starts {-skip:.3f}s early and can't be moved later")
    for name, seconds in corrections.items():
        path = os.path.join(out_dir, name)
        ticks = round(seconds * read_mp4(path)["timescale"])
        if ticks > 0:
            add_edit_list(path, ticks)
            log(f"[mp4] {name}: start moved back {seconds:.3f}s with an edit list")
    return names


def _boxes(buf: bytes, start: int, end: int):
    pos = start
    while pos + 8 <= end:
        size, kind = struct.unpack(">I4s", buf[pos:pos + 8])
        if size < 8 or pos + size > end:
            raise ValueError("bad MP4 box")
        yield kind.decode("latin-1"), pos, size
        pos += size


def add_edit_list(path: str, media_time: int) -> None:
    """
    Add an edit list to a single-track fragmented MP4 (written by
    split_video): presentation starts media_time ticks into the media.
    Everything after the header moves along by the same amount; fragment
    offsets are relative (default_base_moof, sidx), so nothing else changes.
    """
    with open(path, "rb") as f:
        head = f.read(64 * 1024)
    moov = next(((pos, size) for kind, pos, size in _top_boxes(head) if kind == "moov"), None)
    if not moov or moov[0] + moov[1] > len(head):
        raise ValueError("no MP4 header in the first 64 KB")
    m_pos, m_size = moov
    trak = next(((pos, size) for kind, pos, size in _boxes(head, m_pos + 8, m_pos + m_size)
                 if kind == "trak"), None)
    if not trak:
        raise ValueError("no track")
    t_pos, t_size = trak
    if any(kind == "edts" for kind, _, _ in _boxes(head, t_pos + 8, t_pos + t_size)):
        raise ValueError("already has an edit list")
    tkhd = next((pos + size for kind, pos, size in _boxes(head, t_pos + 8, t_pos + t_size) if kind == "tkhd"), None)
    if tkhd is None:
        raise ValueError("no track header")
    # One edit: the whole (fragmented, so unknown) duration, starting at media_time
    elst = struct.pack(">I4sI", 28, b"elst", 0) + struct.pack(">IIiHH", 1, 0, media_time, 1, 0)
    edts = struct.pack(">I4s", 8 + len(elst), b"edts") + elst
    new_head = bytearray(head[:tkhd]) + edts + head[tkhd:m_pos + m_size]
    struct.pack_into(">I", new_head, t_pos, t_size + len(edts))
    struct.pack_into(">I", new_head, m_pos, m_size + len(edts))
    tmp = path + ".elst"
    try:
        with open(path, "rb") as src, open(tmp, "wb") as out:
            out.write(new_head)
            src.seek(m_pos + m_size)
            shutil.copyfileobj(src, out, COPY_CHUNK)
        shutil.copymode(path, tmp)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


# -- Reading --------------------------------------------------------------


def _top_boxes(buf: bytes):
    """Top-level boxes wholly or partly in buf (a file's first bytes)."""
    pos = 0
    while pos + 8 <= len(buf):
        size, kind = struct.unpack(">I4s", buf[pos:pos + 8])
        if size == 1 and pos + 16 <= len(buf):
            size = struct.unpack(">Q", buf[pos + 8:pos + 16])[0]
        if size < 8:
            return
        yield kind.decode("latin-1"), pos, size
        pos += size


def _handler(trak: bytes) -> bytes:
    h = trak.find(b"hdlr")
    return trak[h + 12:h + 16] if h >= 4 else b""


def _full_box_int(buf: bytes, at: int, ver: int, v0: str, v1: str, off0: int, off1: int):
    fmt, off = (v0, off0) if ver == 0 else (v1, off1)
    return struct.unpack(fmt, buf[at + off:at + off + struct.calcsize(fmt)])[0]


def _track_info(trak: bytes) -> dict:
    info = {"handler": _handler(trak).decode("latin-1"), "track_id": None, "timescale": 0,
            "media_time": 0, "codec": ""}
    t = trak.find(b"tkhd")
    if t >= 4:
        ver = trak[t + 4]
        info["track_id"] = _full_box_int(trak, t + 4, ver, ">I", ">I", 12, 20)
    m = trak.find(b"mdhd")
    if m >= 4:
        ver = trak[m + 4]
        info["timescale"] = _full_box_int(trak, m + 4, ver, ">I", ">I", 12, 20)
    e = trak.find(b"elst")
    if e >= 4:
        ver = trak[e + 4]
        count = struct.unpack(">I", trak[e + 8:e + 12])[0]
        if count >= 1:
            # segment_duration, then media_time
            info["media_time"] = struct.unpack(">i", trak[e + 16:e + 20])[0] if ver == 0 else \
                struct.unpack(">q", trak[e + 20:e + 28])[0]
    a = trak.find(b"avcC")
    if a >= 4:
        p = a + 4
        info["codec"] = f"avc1.{trak[p + 1]:02x}{trak[p + 2]:02x}{trak[p + 3]:02x}"
    elif trak.find(b"mp4a") >= 0:
        info["codec"] = "mp4a.40.2"
    return info


def read_mp4(path: str, offset: int = 0, size: int | None = None) -> dict:
    """
    The header and fragment index of a fragmented MP4 at path[offset:offset+size]
    (a file, or a member of an uncompressed package):
      init_size  bytes of initialization data (ftyp + moov) from the start
      fragments  [(byte offset from the start, length, duration in seconds)]
      timescale, media_time (edit list start, in timescale ticks), codec
    Raises ValueError if it isn't a fragmented MP4 with an index.
    """
    if size is None:
        size = os.path.getsize(path) - offset
    with open(path, "rb") as f:
        f.seek(offset)
        head = f.read(min(size, 1024 * 1024))
    init_size = 0
    track = None
    frags = None
    for kind, pos, box_size in _top_boxes(head):
        if kind == "moov":
            if pos + box_size > len(head):
                raise ValueError("MP4 header too large")
            for k, p, s in _boxes(head, pos + 8, pos + box_size):
                if k == "trak":
                    info = _track_info(head[p + 8:p + s])
                    if track is None or info["handler"] == "vide":
                        track = info
            init_size = pos + box_size
        elif kind == "sidx":
            if pos + box_size > len(head):
                raise ValueError("MP4 index too large")
            body = head[pos + 8:pos + box_size]
            ver = body[0]
            ref_id, timescale = struct.unpack(">II", body[4:12])
            if track and track["track_id"] not in (None, ref_id):
                continue
            first_offset, rest = (struct.unpack(">I", body[16:20])[0], body[20:]) if ver == 0 else \
                (struct.unpack(">Q", body[20:28])[0], body[28:])
            count = struct.unpack(">H", rest[2:4])[0]
            at = pos + box_size + first_offset
            frags = []
            for i in range(count):
                ref, dur = struct.unpack(">II", rest[4 + 12 * i:12 + 12 * i])
                if ref & 0x80000000:
                    raise ValueError("nested MP4 index")
                length = ref & 0x7FFFFFFF
                frags.append((at, length, dur / timescale))
                at += length
            if at > size:
                raise ValueError("MP4 index runs past the end of the file")
            break
        elif kind in ("moof", "mdat"):
            break
    if not init_size or not track or not frags:
        raise ValueError("not a fragmented MP4 with an index")
    return {"init_size": init_size, "fragments": frags, "timescale": track["timescale"] or 1,
            "media_time": track["media_time"], "codec": track["codec"]}


def build_media_playlist(info: dict, uri: str, target_seconds: float) -> str:
    """An HLS media playlist of byte ranges of one fragmented MP4, in segments of about target_seconds."""
    segments = []
    cur = None
    for off, length, dur in info["fragments"]:
        if cur and cur[2] < target_seconds and cur[0] + cur[1] == off:
            cur[1] += length
            cur[2] += dur
        else:
            cur = [off, length, dur]
            segments.append(cur)
    target = max(1, int(-(-max(seg[2] for seg in segments) // 1)))
    lines = [
        "#EXTM3U",
        "#EXT-X-VERSION:7",
        f"#EXT-X-TARGETDURATION:{target}",
        "#EXT-X-MEDIA-SEQUENCE:0",
        "#EXT-X-PLAYLIST-TYPE:VOD",
        "#EXT-X-INDEPENDENT-SEGMENTS",
        f'#EXT-X-MAP:URI="{uri}",BYTERANGE="{info["init_size"]}@0"',
    ]
    for off, length, dur in segments:
        lines.append(f"#EXTINF:{dur:.6f},")
        lines.append(f"#EXT-X-BYTERANGE:{length}@{off}")
        lines.append(uri)
    lines.append("#EXT-X-ENDLIST")
    return "\n".join(lines) + "\n"


def build_master_playlist(video_uri: str, video_info: dict, video_bytes: int,
                          audio: list[tuple[str, str, int]]) -> str:
    """
    An HLS master playlist: the video, with each audio file as an alternate
    audio track. audio is [(playlist URI, display name, bytes)] in track order.
    """
    duration = sum(f[2] for f in video_info["fragments"]) or 1.0
    total = video_bytes + (max(a[2] for a in audio) if audio else 0)
    bandwidth = int(total * 8 / duration * 1.25)
    codecs = [video_info["codec"] or "avc1.640028"]
    lines = ["#EXTM3U", "#EXT-X-VERSION:7", "#EXT-X-INDEPENDENT-SEGMENTS"]
    if audio:
        codecs.append("mp4a.40.2")
        for i, (uri, name, _) in enumerate(audio):
            default = "YES" if i == 0 else "NO"
            lines.append(f'#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="{name}",DEFAULT={default},'
                         f'AUTOSELECT={default},URI="{uri}"')
        lines.append(f'#EXT-X-STREAM-INF:BANDWIDTH={bandwidth},CODECS="{",".join(codecs)}",AUDIO="audio"')
    else:
        lines.append(f'#EXT-X-STREAM-INF:BANDWIDTH={bandwidth},CODECS="{codecs[0]}"')
    lines.append(video_uri)
    return "\n".join(lines) + "\n"

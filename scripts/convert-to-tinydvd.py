#!/usr/bin/env python3
"""
convert-to-tinydvd: tinydvd Converter & Packager CLI
Decomposes DVD title sets and menus into compact Meta-VOB (.mvob) sector
containers (1 video reference sector per VOBU + NV_PCK + SPU) and
companion H.264 + AAC Matroska (.mkv) streams.
"""

import sys
import os
import glob
import struct
import argparse
import subprocess
import shutil
import tempfile
import contextlib
import zipfile

SECTOR_SIZE = 2048
MVOB_MAGIC = 0x4D564F42  # "MVOB" in little-endian uint32
MVOB_VERSION = 1

# Struct format:
# magic (uint32)
# version (uint16)
# vts_id (uint16)
# vobu_index (uint32)
# pts_nanos (uint64)
# duration_nanos (uint32)
# is_idr (uint8)
# reserved (15 bytes)
MVOB_STRUCT = struct.Struct("<IHHIQIB15s")
assert MVOB_STRUCT.size == 40, f"Expected struct size 40, got {MVOB_STRUCT.size}"


def pack_mvob_video_ref(vts_id, vobu_index, pts_nanos, duration_nanos, is_idr=1):
    return MVOB_STRUCT.pack(
        MVOB_MAGIC,
        MVOB_VERSION,
        int(vts_id),
        int(vobu_index),
        max(0, int(pts_nanos)),
        max(0, int(duration_nanos)),
        1 if is_idr else 0,
        b"\x00" * 15
    )


def is_pack_header(sector):
    return len(sector) >= 14 and sector[:4] == b"\x00\x00\x01\xba"


def is_nv_pck(sector):
    if not is_pack_header(sector) or len(sector) < SECTOR_SIZE:
        return False
    # NV_PCK contains PCI PES packet with stream ID 0xBF and substream ID 0x00
    return sector[38:42] == b"\x00\x00\x01\xbf" and sector[44] == 0x00


def parse_nv_pck_timestamps(sector):
    """
    Extract vobu_s_ptm and vobu_e_ptm from a PCI packet in NV_PCK.
    vobu_s_ptm is at sector offset 57..61 (90kHz ticks)
    vobu_e_ptm is at sector offset 61..65 (90kHz ticks)
    """
    if not is_nv_pck(sector):
        return None, None
    s_ptm = struct.unpack(">I", sector[57:61])[0]
    e_ptm = struct.unpack(">I", sector[61:65])[0]
    return s_ptm, e_ptm


def parse_nv_pck_dsi_ids(sector):
    """
    Extract (vob_id, c_id) from the DSI packet in NV_PCK (at sector offset 1024).
    Returns (0, 0) if DSI packet header is not present.
    """
    if len(sector) < SECTOR_SIZE:
        return 0, 0
    if sector[1024:1028] == b"\x00\x00\x01\xbf" and sector[1030] == 0x01:
        vob_id = struct.unpack(">H", sector[1055:1057])[0]
        c_id = sector[1058]
        return vob_id, c_id
    return 0, 0


def _get_pes_stream_and_substream(sector):
    if not is_pack_header(sector) or len(sector) < 24:
        return None, None
    pack_stuffing = sector[13] & 0x07
    pes_offset = 14 + pack_stuffing
    if pes_offset + 9 > len(sector):
        return None, None
    if sector[pes_offset:pes_offset + 3] != b"\x00\x00\x01":
        return None, None
    stream_id = sector[pes_offset + 3]
    if stream_id in (0xBB, 0xBE, 0xBF):
        return stream_id, None
    pes_header_data_len = sector[pes_offset + 8]
    payload_start = pes_offset + 9 + pes_header_data_len
    substream_id = sector[payload_start] if payload_start < len(sector) else None
    return stream_id, substream_id


def is_video_sector(sector):
    stream_id, _ = _get_pes_stream_and_substream(sector)
    return stream_id is not None and (0xE0 <= stream_id <= 0xEF)


def is_spu_sector(sector):
    stream_id, substream_id = _get_pes_stream_and_substream(sector)
    return stream_id == 0xBD and substream_id is not None and (0x20 <= substream_id <= 0x3F)


def is_audio_sector(sector):
    stream_id, substream_id = _get_pes_stream_and_substream(sector)
    if stream_id is None:
        return False
    if 0xC0 <= stream_id <= 0xDF:
        return True
    if stream_id == 0xBD and substream_id is not None:
        return (0x80 <= substream_id <= 0x8F) or (0xA0 <= substream_id <= 0xAF)
    return False


def hollow_video_sector(sector, ref_bytes):
    """
    Replaces video payload with mvob_video_ref_t and zeroes out to 2048 bytes.
    """
    pack_stuffing = sector[13] & 0x07
    pes_offset = 14 + pack_stuffing
    if pes_offset + 9 > len(sector):
        return create_dummy_video_sector(ref_bytes)

    pes_header_data_len = sector[pes_offset + 8]
    payload_start = pes_offset + 9 + pes_header_data_len
    if payload_start + len(ref_bytes) > SECTOR_SIZE:
        return create_dummy_video_sector(ref_bytes)

    # Keep original pack header and PES header up to payload_start
    hollowed = bytearray(sector[:payload_start])
    hollowed.extend(ref_bytes)
    if len(hollowed) < SECTOR_SIZE:
        hollowed.extend(b"\x00" * (SECTOR_SIZE - len(hollowed)))
    return bytes(hollowed[:SECTOR_SIZE])


def create_dummy_video_sector(ref_bytes):
    buf = bytearray(SECTOR_SIZE)
    # Pack header (14 bytes)
    buf[0:4] = b"\x00\x00\x01\xba"
    buf[4:13] = b"\x44\x00\x04\x00\x04\x01\x01\x89\xc3"
    buf[13] = 0xF8
    # PES video header (9 bytes)
    buf[14:18] = b"\x00\x00\x01\xe0"
    buf[18:20] = struct.pack(">H", SECTOR_SIZE - 20)
    buf[20] = 0x80
    buf[21] = 0x00
    buf[22] = 0x00
    # Payload starts at 23
    buf[23:23 + len(ref_bytes)] = ref_bytes
    return bytes(buf)


def scan_vts_vobus(vob_paths, fps, return_segments=False):
    """
    Scan all NV_PCK headers across VOB files to record VOBU start times, frame indices,
    and contiguous VOB_ID segments.
    Ensures continuous, strictly non-negative timestamps across multi-VOB and VOB_ID boundaries.
    """
    vobus = []
    segments = []
    base_time_sec = 0.0
    segment_start_ptm = None
    last_s_ptm = None
    last_vob_id = None
    last_vobu_end_sec = 0.0
    current_seg = None

    for vob_path in vob_paths:
        print(f"[scan] Analyzing NV_PCKs in {os.path.basename(vob_path)}...")
        file_offset = 0
        with open(vob_path, "rb") as f:
            while True:
                sector = f.read(SECTOR_SIZE)
                if len(sector) < SECTOR_SIZE:
                    break
                if is_nv_pck(sector):
                    s_ptm, e_ptm = parse_nv_pck_timestamps(sector)
                    vob_id, c_id = parse_nv_pck_dsi_ids(sector)
                    duration_sec = (e_ptm - s_ptm) / 90000.0 if e_ptm >= s_ptm else 0.5
                    if duration_sec <= 0:
                        duration_sec = 0.5

                    new_segment = False
                    if segment_start_ptm is None:
                        new_segment = True
                    elif vob_id != 0 and last_vob_id not in (None, 0) and vob_id != last_vob_id:
                        new_segment = True
                    elif last_s_ptm is not None and s_ptm < last_s_ptm:
                        new_segment = True

                    if new_segment:
                        if current_seg is not None:
                            segments.append(current_seg)
                        base_time_sec = last_vobu_end_sec
                        segment_start_ptm = s_ptm
                        time_sec = base_time_sec
                        current_seg = {
                            "vob_id": vob_id,
                            "vob_path": vob_path,
                            "start_byte": file_offset,
                            "end_byte": file_offset + SECTOR_SIZE,
                            "first_s_ptm": s_ptm,
                            "last_e_ptm": e_ptm,
                            "base_time_sec": base_time_sec,
                            "duration_sec": duration_sec,
                            "vobu_count": 1,
                            "video_sectors": 0,
                            "audio_sectors": 0,
                        }
                    else:
                        time_sec = base_time_sec + max(0.0, (s_ptm - segment_start_ptm) / 90000.0)
                        if current_seg is not None:
                            current_seg["end_byte"] = file_offset + SECTOR_SIZE
                            current_seg["last_e_ptm"] = e_ptm
                            current_seg["duration_sec"] = (time_sec + duration_sec) - current_seg["base_time_sec"]
                            current_seg["vobu_count"] += 1

                    last_s_ptm = s_ptm
                    last_vob_id = vob_id
                    last_vobu_end_sec = time_sec + duration_sec

                    pts_nanos = max(0, int(round(time_sec * 1e9)))
                    duration_nanos = max(0, int(round(duration_sec * 1e9)))
                    frame_idx = max(0, int(round(time_sec * fps)))

                    vobus.append({
                        "index": len(vobus),
                        "vob_id": vob_id,
                        "c_id": c_id,
                        "s_ptm": s_ptm,
                        "e_ptm": e_ptm,
                        "time_sec": time_sec,
                        "frame_idx": frame_idx,
                        "pts_nanos": pts_nanos,
                        "duration_nanos": duration_nanos,
                    })
                else:
                    if current_seg is not None:
                        current_seg["end_byte"] = file_offset + SECTOR_SIZE
                        if is_video_sector(sector):
                            current_seg["video_sectors"] += 1
                        elif is_audio_sector(sector):
                            current_seg["audio_sectors"] += 1

                file_offset += SECTOR_SIZE

    if current_seg is not None:
        segments.append(current_seg)

    # Deduplicate frame indices
    unique_frames = []
    seen = set()
    for v in vobus:
        f = v["frame_idx"]
        if f not in seen:
            seen.add(f)
            unique_frames.append(f)
    unique_frames.sort()

    if return_segments:
        return vobus, unique_frames, segments
    return vobus, unique_frames


def generate_qpfile(keyframes, output_path):
    """
    Emit vobu_keyframes.txt formatted for x264:
    0 I -1
    15 I -1
    ...
    """
    with open(output_path, "w") as f:
        for fidx in keyframes:
            f.write(f"{fidx} I -1\n")
    print(f"[transcode] Wrote {len(keyframes)} keyframe points to {output_path}")


def _transcode_segmented_vob(segments, output_mkv, crf=18, preset="slow", sar="32:27"):
    """
    Transcodes a menu or multi-segment VOB where individual VOB_IDs may be
    still-frame menus (1 I-frame held for N seconds) or have discontinuous PTS.
    Each VOB_ID segment is stretched/trimmed to its exact NV_PCK duration using tpad,
    with either its native audio stream or silent stereo AAC, then concatenated.
    """
    with tempfile.TemporaryDirectory(prefix="tinydvd_seg_") as tmpdir:
        seg_mkvs = []
        for idx, seg in enumerate(segments):
            dur = max(0.1, float(seg["duration_sec"]))
            seg_vob = os.path.join(tmpdir, f"seg_{idx:03d}.vob")
            seg_mkv = os.path.join(tmpdir, f"seg_{idx:03d}.mkv")

            with open(seg["vob_path"], "rb") as fin, open(seg_vob, "wb") as fout:
                fin.seek(seg["start_byte"])
                remaining = seg["end_byte"] - seg["start_byte"]
                while remaining > 0:
                    chunk = fin.read(min(1024 * 1024, remaining))
                    if not chunk:
                        break
                    fout.write(chunk)
                    remaining -= len(chunk)

            vf = f"tpad=stop_mode=clone:stop_duration={dur:.6f},fps=30000/1001,trim=duration={dur:.6f},setpts=PTS-STARTPTS"
            if seg["audio_sectors"] > 0:
                cmd = [
                    "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                    "-probesize", "50M", "-analyzeduration", "50M",
                    "-f", "mpeg", "-i", seg_vob,
                    "-vf", vf,
                    "-af", f"apad=whole_dur={dur:.6f}",
                    "-map", "0:v:0", "-map", "0:a:0",
                    "-c:v", "libx264", "-crf", str(crf), "-preset", preset,
                    "-x264-params", "keyint=15:min-keyint=1:open-gop=0:scenecut=0",
                    "-sar", sar,
                    "-c:a", "aac", "-b:a", "192k", "-ac", "2", "-ar", "48000",
                    seg_mkv
                ]
            else:
                cmd = [
                    "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                    "-probesize", "50M", "-analyzeduration", "50M",
                    "-f", "mpeg", "-i", seg_vob,
                    "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo",
                    "-vf", vf,
                    "-map", "0:v:0", "-map", "1:a:0",
                    "-t", f"{dur:.6f}",
                    "-c:v", "libx264", "-crf", str(crf), "-preset", preset,
                    "-x264-params", "keyint=15:min-keyint=1:open-gop=0:scenecut=0",
                    "-sar", sar,
                    "-c:a", "aac", "-b:a", "192k", "-ac", "2", "-ar", "48000",
                    seg_mkv
                ]

            res = subprocess.run(cmd)
            if res.returncode != 0:
                raise RuntimeError(f"FFmpeg segment transcode failed for segment {idx} (VOB_ID {seg['vob_id']})")
            seg_mkvs.append(seg_mkv)

        if len(seg_mkvs) == 1:
            shutil.move(seg_mkvs[0], output_mkv)
        else:
            concat_txt = os.path.join(tmpdir, "concat.txt")
            with open(concat_txt, "w") as f:
                for m in seg_mkvs:
                    f.write(f"file '{m}'\n")
            cmd_concat = [
                "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                "-f", "concat", "-safe", "0", "-i", concat_txt,
                "-c", "copy", output_mkv
            ]
            res = subprocess.run(cmd_concat)
            if res.returncode != 0:
                raise RuntimeError("FFmpeg segment concatenation failed")

    print(f"[transcode] Output generated (segmented): {output_mkv}")


def transcode_video(vob_paths, qpfile_path, output_mkv, keyframe_times=None, crf=18, preset="slow", tune="film", sar="32:27", segments=None, is_menu=False):
    """
    Run ffmpeg to encode companion video + audio into an MKV container.
    - Uses per-segment tpad encoding for menus or still-frame VOBs so every VOB_ID
      maps 1:1 to its NV_PCK timeline.
    - Uses byte-level concat: protocol for multi-file title VOBs (VTS_xx_1..N.VOB)
      to preserve continuous MPEG-PS timestamps without the 26-hour -f concat bug.
    """
    if segments and len(vob_paths) == 1 and (
        is_menu or os.path.getsize(vob_paths[0]) < 5 * 1024 * 1024
    ):
        print(f"[ffmpeg] Encoding {len(segments)} VOB_ID segment(s) with still-frame/menu timeline preservation -> {os.path.basename(output_mkv)}")
        _transcode_segmented_vob(segments, output_mkv, crf=crf, preset=preset, sar=sar)
        return

    if len(vob_paths) == 1:
        input_spec = os.path.abspath(vob_paths[0])
    else:
        input_spec = "concat:" + "|".join(os.path.abspath(p) for p in vob_paths)

    keyframe_args = []
    if keyframe_times:
        if len(keyframe_times) <= 400:
            keyframe_args = ["-force_key_frames", ",".join(keyframe_times)]
        else:
            # Avoid giant command lines while still placing keyframes at segment boundaries
            if segments:
                seg_times = [f"{s['base_time_sec']:.3f}" for s in segments]
                keyframe_args = ["-force_key_frames", ",".join(seg_times)]

    cmd = [
        "ffmpeg", "-y",
        "-probesize", "50M", "-analyzeduration", "50M",
        "-i", input_spec,
        "-map", "0:v:0",
        "-map", "0:a?",
        "-c:v", "libx264",
        "-crf", str(crf),
        "-preset", preset,
        "-tune", tune,
        *keyframe_args,
        "-x264-params", "keyint=30:min-keyint=1:open-gop=0:scenecut=0",
        "-sar", sar,
        "-c:a", "aac",
        "-b:a", "192k",
        "-ac", "2",
        "-ar", "48000",
        output_mkv
    ]

    print("[ffmpeg] Running:", " ".join(cmd[:14]), "...")
    res = subprocess.run(cmd)
    if res.returncode != 0:
        raise RuntimeError(f"FFmpeg transcode failed with exit code {res.returncode}")
    print(f"[transcode] Output generated: {output_mkv}")


def _make_ref_bytes_for_vobu(vts_id, vobu):
    pts_nanos = vobu["pts_nanos"] if vobu else 0
    duration_nanos = vobu["duration_nanos"] if vobu else 500000000
    idx = vobu["index"] if vobu else 0
    return pack_mvob_video_ref(
        vts_id=vts_id,
        vobu_index=idx,
        pts_nanos=pts_nanos,
        duration_nanos=duration_nanos,
        is_idr=1
    )


def emit_mvob_files(vob_paths, vts_id, vobus, output_dir, keep_audio=False):
    """
    Emit compact VTS_xx_*.mvob (or VIDEO_TS.mvob) files:
    - NV_PCK sectors copied verbatim.
    - All video sectors in a VOBU are collapsed into a SINGLE 2048-byte
      mvob_video_ref_t sector (or a synthetic video ref sector is inserted if
      the VOBU had 0 raw video sectors, e.g. held menu still frames).
    - SPU (subtitle / menu button highlight bitmap) sectors copied verbatim.
    - Audio sectors are stripped by default (keep_audio=False) since audio is
      muxed into the companion .mkv file, shrinking .mvob files by ~98.8%.
    """
    vobu_idx = 0
    current_vobu = None
    wrote_video_ref = True

    for vob_path in vob_paths:
        filename = os.path.basename(vob_path)
        base, _ = os.path.splitext(filename)
        base_upper = base.upper()
        out_mvob = os.path.join(output_dir, f"{base_upper}.mvob")
        out_vob_symlink = os.path.join(output_dir, f"{base_upper}.VOB")

        print(f"[mvob] Writing compact {out_mvob}...")
        with open(vob_path, "rb") as fin, open(out_mvob, "wb") as fout:
            while True:
                sector = fin.read(SECTOR_SIZE)
                if len(sector) < SECTOR_SIZE:
                    break

                if is_nv_pck(sector):
                    # If previous VOBU had no video sectors at all, emit a synthetic ref sector
                    if current_vobu is not None and not wrote_video_ref:
                        ref_bytes = _make_ref_bytes_for_vobu(vts_id, current_vobu)
                        fout.write(create_dummy_video_sector(ref_bytes))

                    # Verbatim write NV_PCK
                    fout.write(sector)
                    if vobu_idx < len(vobus):
                        current_vobu = vobus[vobu_idx]
                        vobu_idx += 1
                    wrote_video_ref = False
                elif is_video_sector(sector):
                    # Write ONLY the first video sector of the VOBU (hollowed with mvob_video_ref_t)
                    # and skip all remaining video sectors in this VOBU!
                    if not wrote_video_ref:
                        ref_bytes = _make_ref_bytes_for_vobu(vts_id, current_vobu)
                        hollowed = hollow_video_sector(sector, ref_bytes)
                        fout.write(hollowed)
                        wrote_video_ref = True
                elif is_spu_sector(sector):
                    # Subpicture / menu button overlay sector: copy verbatim
                    fout.write(sector)
                elif is_audio_sector(sector):
                    if keep_audio:
                        fout.write(sector)
                else:
                    # Other system sectors if any
                    if keep_audio:
                        fout.write(sector)

            # Flush synthetic video ref if the last VOBU in this file had 0 video sectors
            if current_vobu is not None and not wrote_video_ref:
                ref_bytes = _make_ref_bytes_for_vobu(vts_id, current_vobu)
                fout.write(create_dummy_video_sector(ref_bytes))
                wrote_video_ref = True

        # Create symlink VTS_xx_x.VOB -> VTS_xx_x.mvob
        if os.path.exists(out_vob_symlink) or os.path.islink(out_vob_symlink):
            os.remove(out_vob_symlink)
        try:
            os.symlink(f"{base_upper}.mvob", out_vob_symlink)
        except OSError as e:
            print(f"[warning] Could not create symlink {out_vob_symlink}: {e}")


def copy_ifo_and_bup(input_dir, output_dir):
    """Copy all .IFO and .BUP files matching the original DVD structure."""
    patterns = ["*.IFO", "*.ifo", "*.BUP", "*.bup"]
    search_dirs = [input_dir]
    if os.path.isdir(os.path.join(input_dir, "VIDEO_TS")):
        search_dirs.append(os.path.join(input_dir, "VIDEO_TS"))

    copied = 0
    for sdir in search_dirs:
        for pat in patterns:
            for fpath in glob.glob(os.path.join(sdir, pat)):
                dest = os.path.join(output_dir, os.path.basename(fpath).upper())
                shutil.copy2(fpath, dest)
                copied += 1
    print(f"[transcode] Copied {copied} IFO/BUP navigation metadata files.")


def process_vmgm(input_dir, output_dir, fps=29.97, crf=18, preset="slow", sar="32:27", keep_audio=False):
    """Process VIDEO_TS.VOB (VMGM domain) if present."""
    search_dirs = [input_dir]
    if os.path.isdir(os.path.join(input_dir, "VIDEO_TS")):
        search_dirs.append(os.path.join(input_dir, "VIDEO_TS"))

    vmgm_vobs = []
    for sdir in search_dirs:
        m = glob.glob(os.path.join(sdir, "VIDEO_TS.VOB")) or glob.glob(os.path.join(sdir, "video_ts.vob"))
        if m:
            vmgm_vobs.extend(m)
            break

    if not vmgm_vobs:
        return

    print("\n=== Processing VMGM Menu (VIDEO_TS.VOB) ===")
    vobus, _, segments = scan_vts_vobus(vmgm_vobs, fps, return_segments=True)
    if vobus:
        output_mkv = os.path.join(output_dir, "VIDEO_TS_VIDEO.mkv")
        transcode_video(
            vob_paths=vmgm_vobs,
            qpfile_path=None,
            output_mkv=output_mkv,
            crf=crf,
            preset=preset,
            sar=sar,
            segments=segments,
            is_menu=True
        )
        emit_mvob_files(vmgm_vobs, 0, vobus, output_dir, keep_audio=keep_audio)


def process_vts(input_dir, output_dir, vts_id, fps=29.97, crf=18, preset="slow", tune="film", sar="32:27", keep_audio=False):
    print(f"\n=== Processing Video Title Set VTS_{vts_id:02d} ===")

    search_dirs = [input_dir]
    if os.path.isdir(os.path.join(input_dir, "VIDEO_TS")):
        search_dirs.append(os.path.join(input_dir, "VIDEO_TS"))

    vob_files = []
    for sdir in search_dirs:
        matched = sorted(glob.glob(os.path.join(sdir, f"VTS_{vts_id:02d}_[1-9]*.VOB")))
        if not matched:
            matched = sorted(glob.glob(os.path.join(sdir, f"VTS_{vts_id:02d}_[1-9]*.vob")))
        if matched:
            vob_files.extend(matched)
            break

    menu_vobs = []
    for sdir in search_dirs:
        m = glob.glob(os.path.join(sdir, f"VTS_{vts_id:02d}_0.VOB")) or glob.glob(os.path.join(sdir, f"VTS_{vts_id:02d}_0.vob"))
        if m:
            menu_vobs.extend(m)
            break

    if not vob_files and not menu_vobs:
        print(f"Notice: No VOB files found for VTS_{vts_id:02d}")
        return

    # If menu VOB exists, transcode VTS_xx_0_VIDEO.mkv and emit compact VTS_xx_0.mvob
    if menu_vobs:
        print(f"[transcode] Processing menu VOB: {menu_vobs[0]}")
        menu_vobus, _, menu_segments = scan_vts_vobus(menu_vobs, fps, return_segments=True)
        if menu_vobus:
            menu_mkv = os.path.join(output_dir, f"VTS_{vts_id:02d}_0_VIDEO.mkv")
            transcode_video(
                vob_paths=menu_vobs,
                qpfile_path=None,
                output_mkv=menu_mkv,
                crf=crf,
                preset=preset,
                sar=sar,
                segments=menu_segments,
                is_menu=True
            )
            emit_mvob_files(menu_vobs, vts_id, menu_vobus, output_dir, keep_audio=keep_audio)

    if not vob_files:
        return

    # Scan VOBU keyframes and segments
    vobus, keyframes, segments = scan_vts_vobus(vob_files, fps, return_segments=True)
    if not keyframes:
        keyframes = [0]

    qpfile_path = os.path.join(output_dir, f"vts_{vts_id:02d}_keyframes.txt")
    generate_qpfile(keyframes, qpfile_path)

    # Encode MKV
    keyframe_times = [f"{v['time_sec']:.3f}" for v in vobus]
    seen_times = set()
    unique_times = []
    for t in keyframe_times:
        if t not in seen_times:
            seen_times.add(t)
            unique_times.append(t)

    output_mkv = os.path.join(output_dir, f"VTS_{vts_id:02d}_VIDEO.mkv")
    transcode_video(
        vob_paths=vob_files,
        qpfile_path=qpfile_path,
        output_mkv=output_mkv,
        keyframe_times=unique_times,
        crf=crf,
        preset=preset,
        tune=tune,
        sar=sar,
        segments=segments,
        is_menu=False
    )

    # Emit compact MVOB files
    emit_mvob_files(vob_files, vts_id, vobus, output_dir, keep_audio=keep_audio)


@contextlib.contextmanager
def resolve_dvd_source(input_path):
    """
    Resolves input_path to a directory containing DVD files:
    - If a directory is provided, yields it directly.
    - If a single .VOB or .IFO file is provided, yields its containing folder.
    - If an ISO (.iso, .img) is provided, automatically mounts it (on macOS/Linux)
      or extracts it, and cleans up on exit.
    """
    if os.path.isdir(input_path):
        yield input_path
        return

    if os.path.isfile(input_path):
        filename = os.path.basename(input_path).upper()
        if filename.endswith(".VOB") or filename.endswith(".IFO") or filename.endswith(".BUP"):
            parent = os.path.dirname(os.path.abspath(input_path))
            print(f"[source] File passed; using parent directory: {parent}")
            yield parent
            return

        ext = os.path.splitext(input_path)[1].lower()
        if ext in [".iso", ".img", ".nrg"]:
            print(f"[source] ISO file detected: {input_path}")
            mountpoint = None
            if sys.platform == "darwin":
                cmd = ["hdiutil", "attach", "-nobrowse", "-readonly", input_path]
                res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                if res.returncode == 0:
                    for line in res.stdout.strip().splitlines():
                        parts = [p.strip() for p in line.split("\t") if p.strip()]
                        for part in reversed(parts):
                            if os.path.isdir(part):
                                mountpoint = part
                                break
                        if mountpoint:
                            break
                    if not mountpoint:
                        tokens = res.stdout.strip().split()
                        for tok in reversed(tokens):
                            if os.path.isdir(tok):
                                mountpoint = tok
                                break
            elif sys.platform.startswith("linux"):
                if shutil.which("7z"):
                    tmp_extract = tempfile.mkdtemp(prefix="tinydvd_iso_")
                    res = subprocess.run(["7z", "x", f"-o{tmp_extract}", input_path],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    if res.returncode == 0:
                        mountpoint = tmp_extract

            if mountpoint and os.path.isdir(mountpoint):
                print(f"[source] Successfully mounted ISO at: {mountpoint}")
                try:
                    yield mountpoint
                finally:
                    print(f"[source] Cleaning up mount: {mountpoint}")
                    if sys.platform == "darwin":
                        subprocess.run(["hdiutil", "detach", mountpoint, "-force"],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    elif mountpoint.startswith("/tmp/tinydvd_iso_") or "tinydvd_iso_" in mountpoint:
                        shutil.rmtree(mountpoint, ignore_errors=True)
                return

    raise ValueError(f"Could not resolve DVD source at '{input_path}'. Must be a directory, an ISO image, or a VOB/IFO file.")


def is_already_converted_dir(path):
    """Returns True if path is a directory that already contains .mvob and _VIDEO.mkv files."""
    if not os.path.isdir(path):
        return False
    entries = os.listdir(path)
    has_mvob = any(f.lower().endswith(".mvob") for f in entries)
    has_mkv  = any(f.lower().endswith("_video.mkv") for f in entries)
    return has_mvob and has_mkv


def ensure_thumbnail_in_staging(staging_dir):
    """Ensure a high-quality poster/menu thumbnail.png exists in staging directory."""
    thumb_path = os.path.join(staging_dir, "thumbnail.png")
    if os.path.exists(thumb_path) and os.path.getsize(thumb_path) > 0:
        return
    files = [f for f in os.listdir(staging_dir) if f.lower().endswith(".mkv")]
    if not files:
        return
    # Prefer menu video with content (> 100KB), else main title video
    menus = [f for f in files if ("_0_video.mkv" in f.lower() or "video_ts_video.mkv" in f.lower())
             and os.path.getsize(os.path.join(staging_dir, f)) > 100000]
    target_mkv = os.path.join(staging_dir, menus[0] if menus else files[0])
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-ss", "00:00:02",
        "-i", target_mkv,
        "-vframes", "1",
        "-q:v", "2",
        thumb_path
    ]
    try:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def package_tdvd_archive(staging_dir, output_archive_path):
    """
    Packages a tinydvd directory into a single uncompressed ZIP_STORED (.tdvd / .tinydvd)
    archive with O(1) central directory lookup and contiguous byte ranges for zero-copy playback.
    """
    parent_dir = os.path.dirname(os.path.abspath(output_archive_path))
    if parent_dir:
        os.makedirs(parent_dir, exist_ok=True)

    ensure_thumbnail_in_staging(staging_dir)

    all_files = []
    for fname in sorted(os.listdir(staging_dir)):
        fpath = os.path.join(staging_dir, fname)
        if os.path.islink(fpath) or not os.path.isfile(fpath):
            continue
        ext = os.path.splitext(fname)[1].lower()
        if ext in (".ifo", ".bup"):
            prio = 0
        elif ext in (".png", ".jpg", ".jpeg"):
            prio = 1
        elif ext == ".mvob":
            prio = 2
        elif ext == ".mkv":
            prio = 3
        elif ext == ".txt":
            prio = 4
        else:
            continue
        all_files.append((prio, fname, fpath))

    all_files.sort(key=lambda item: (item[0], item[1]))
    print(f"\n[package] Creating single-file archive {output_archive_path} ({len(all_files)} constituent files, ZIP_STORED)...")
    with zipfile.ZipFile(output_archive_path, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as zf:
        for _, fname, fpath in all_files:
            zf.write(fpath, arcname=fname)
    total_mb = os.path.getsize(output_archive_path) / (1024.0 * 1024.0)
    print(f"[package] Wrote {output_archive_path} ({total_mb:.1f} MB)")


def run_conversion_to_dir(raw_input, target_dir, args):
    os.makedirs(target_dir, exist_ok=True)
    with resolve_dvd_source(raw_input) as input_path:
        copy_ifo_and_bup(input_path, target_dir)

        if args.vts is not None:
            if args.vts == 0:
                process_vmgm(input_path, target_dir, args.fps, args.crf, args.preset, args.sar, keep_audio=args.keep_mvob_audio)
            else:
                process_vts(input_path, target_dir, args.vts, args.fps, args.crf, args.preset, args.tune, args.sar, keep_audio=args.keep_mvob_audio)
        else:
            process_vmgm(input_path, target_dir, args.fps, args.crf, args.preset, args.sar, keep_audio=args.keep_mvob_audio)

            vts_ids = set()
            for root, _, files in os.walk(input_path):
                for f in files:
                    f_upper = f.upper()
                    if f_upper.startswith("VTS_") and (f_upper.endswith(".VOB") or f_upper.endswith(".IFO")):
                        parts = f_upper.split("_")
                        if len(parts) >= 2 and parts[1].isdigit():
                            vts_ids.add(int(parts[1]))

            if not vts_ids:
                vts_ids = {1}

            for vid in sorted(vts_ids):
                process_vts(input_path, target_dir, vid, args.fps, args.crf, args.preset, args.tune, args.sar, keep_audio=args.keep_mvob_audio)


def main():
    parser = argparse.ArgumentParser(
        prog="convert-to-tinydvd",
        description="convert-to-tinydvd: Converts a DVD directory or ISO into a single-file .tdvd archive (or directory)."
    )
    parser.add_argument("input", help="Source DVD directory, ISO file, or existing tinydvd directory")
    parser.add_argument("output", help="Output .tdvd / .tinydvd file (or output directory)")
    parser.add_argument("--vts", type=int, default=None, help="Specific VTS title set ID to process (default: all)")
    parser.add_argument("--fps", type=float, default=29.97, help="Disc frame rate (default: 29.97)")
    parser.add_argument("--crf", type=int, default=18, help="x264 CRF quality level (default: 18)")
    parser.add_argument("--preset", default="slow", help="x264 encoding preset (default: slow)")
    parser.add_argument("--tune", default="film", help="x264 tune option (default: film)")
    parser.add_argument("--sar", default="32:27", help="Storage aspect ratio (default: 32:27 for NTSC anamorphic)")
    parser.add_argument("--keep-mvob-audio", action="store_true", help="Also retain raw audio sectors inside .mvob files")

    args = parser.parse_args()

    raw_input = os.path.abspath(args.input)
    output_path = os.path.abspath(args.output)
    is_single_file = output_path.lower().endswith((".tdvd", ".tinydvd"))

    print("convert-to-tinydvd")
    print(f"Input:  {raw_input}")
    print(f"Output: {output_path}")

    if is_single_file:
        if is_already_converted_dir(raw_input):
            package_tdvd_archive(raw_input, output_path)
        else:
            parent_dir = os.path.dirname(output_path) or "."
            os.makedirs(parent_dir, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix=".tdvd_stage_", dir=parent_dir) as stage_dir:
                run_conversion_to_dir(raw_input, stage_dir, args)
                package_tdvd_archive(stage_dir, output_path)
    else:
        run_conversion_to_dir(raw_input, output_path, args)

    print("\nConversion completed successfully!")


if __name__ == "__main__":
    main()

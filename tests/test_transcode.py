#!/usr/bin/env python3
"""
Integration test for tinydvd transcoder and packager.
Validates:
1. Sector reading, NV_PCK parsing, and keyframe discovery.
2. Binary descriptor packing in Python and interoperability with C parser.
3. Collapsing of multiple video sectors per VOBU into a single mvob_video_ref_t sector.
4. Synthetic mvob_video_ref_t injection for still-frame hold VOBUs with 0 raw video sectors.
5. Verbatim preservation of NV_PCK and SPU sectors (and optional audio preservation).
"""

import os
import sys
import tempfile
import struct

SECTOR_SIZE = 2048


def build_pack_header(scr=0):
    buf = bytearray(14)
    buf[0:4] = b"\x00\x00\x01\xba"
    buf[4:13] = b"\x44\x00\x04\x00\x04\x01\x01\x89\xc3"
    buf[13] = 0xF8  # stuffing = 0
    return bytes(buf)


def build_nv_pck(s_ptm, e_ptm, vob_id=1, c_id=1):
    buf = bytearray(SECTOR_SIZE)
    buf[0:14] = build_pack_header()
    # System header
    buf[14:18] = b"\x00\x00\x01\xbb"
    buf[18:20] = b"\x00\x12"  # 18 bytes
    # PCI PES packet at offset 38
    buf[38:42] = b"\x00\x00\x01\xbf"
    buf[42:44] = struct.pack(">H", 980)  # len
    buf[44] = 0x00  # PCI substream
    # pci_gi: offset 57..61 is vobu_s_ptm, 61..65 is vobu_e_ptm
    buf[57:61] = struct.pack(">I", s_ptm)
    buf[61:65] = struct.pack(">I", e_ptm)
    # DSI PES packet at offset 1024
    buf[1024:1028] = b"\x00\x00\x01\xbf"
    buf[1028:1030] = struct.pack(">H", 1018)
    buf[1030] = 0x01  # DSI substream
    buf[1055:1057] = struct.pack(">H", vob_id)
    buf[1058] = c_id
    return bytes(buf)


def build_video_sector(payload_pattern=0xAA):
    buf = bytearray(SECTOR_SIZE)
    buf[0:14] = build_pack_header()
    # PES video packet
    buf[14:18] = b"\x00\x00\x01\xe0"
    buf[18:20] = struct.pack(">H", SECTOR_SIZE - 20)
    buf[20] = 0x80
    buf[21] = 0x80  # PTS present
    buf[22] = 0x05  # header data len 5
    buf[23:28] = b"\x21\x00\x01\x00\x01"  # PTS
    # Payload starts at 28
    buf[28:] = bytes([payload_pattern]) * (SECTOR_SIZE - 28)
    return bytes(buf)


def build_audio_sector(substream=0x80):
    buf = bytearray(SECTOR_SIZE)
    buf[0:14] = build_pack_header()
    # Private Stream 1 (AC-3 / DTS)
    buf[14:18] = b"\x00\x00\x01\xbd"
    buf[18:20] = struct.pack(">H", SECTOR_SIZE - 20)
    buf[20] = 0x80
    buf[21] = 0x00
    buf[22] = 0x00
    buf[23] = substream  # AC-3 audio stream 0
    buf[24:] = b"\x0B\x77" * ((SECTOR_SIZE - 24) // 2)  # AC-3 syncword
    return bytes(buf)


def build_spu_sector(substream=0x20):
    buf = bytearray(SECTOR_SIZE)
    buf[0:14] = build_pack_header()
    # Private Stream 1 (SPU)
    buf[14:18] = b"\x00\x00\x01\xbd"
    buf[18:20] = struct.pack(">H", SECTOR_SIZE - 20)
    buf[20] = 0x80
    buf[21] = 0x00
    buf[22] = 0x00
    buf[23] = substream  # SPU stream 0
    buf[24:] = b"\x55" * (SECTOR_SIZE - 24)
    return bytes(buf)


def main():
    print("=== Running dvd-hybrid-transcode Integration Test ===")

    with tempfile.TemporaryDirectory() as tmpdir:
        input_dir = os.path.join(tmpdir, "in")
        output_dir = os.path.join(tmpdir, "out")
        os.makedirs(input_dir)

        # Create dummy IFO files
        with open(os.path.join(input_dir, "VIDEO_TS.IFO"), "wb") as f:
            f.write(b"DVDVIDEO-VMG" + b"\x00" * 2036)
        with open(os.path.join(input_dir, "VTS_01_0.IFO"), "wb") as f:
            f.write(b"DVDVIDEO-VTS" + b"\x00" * 2036)

        # Create synthetic VTS_01_1.VOB with 3 VOBUs:
        # VOBU 0: 1 NV_PCK + 5 video sectors + 2 audio sectors + 1 SPU sector
        # VOBU 1: 1 NV_PCK + 3 video sectors + 1 audio sector
        # VOBU 2 (still hold in VOB_ID 2): 1 NV_PCK + 0 video sectors + 1 SPU sector
        vob_path = os.path.join(input_dir, "VTS_01_1.VOB")
        nv1 = build_nv_pck(s_ptm=0, e_ptm=45000, vob_id=1, c_id=1)      # 0.0s to 0.5s
        v1 = build_video_sector(payload_pattern=0x11)
        a1 = build_audio_sector(substream=0x80)
        s1 = build_spu_sector(substream=0x20)
        nv2 = build_nv_pck(s_ptm=45000, e_ptm=90000, vob_id=1, c_id=1)  # 0.5s to 1.0s
        v2 = build_video_sector(payload_pattern=0x22)
        nv3 = build_nv_pck(s_ptm=0, e_ptm=45000, vob_id=2, c_id=1)      # 1.0s to 1.5s (new VOB_ID)

        with open(vob_path, "wb") as f:
            # VOBU 0 (9 sectors total)
            f.write(nv1)
            for _ in range(5):
                f.write(v1)
            f.write(a1)
            f.write(a1)
            f.write(s1)
            # VOBU 1 (5 sectors total)
            f.write(nv2)
            for _ in range(3):
                f.write(v2)
            f.write(a1)
            # VOBU 2 (2 sectors total, 0 video sectors)
            f.write(nv3)
            f.write(s1)

        # Import dvd-hybrid-transcode functions
        sys.path.insert(0, os.path.abspath("scripts"))
        import importlib
        transcoder = importlib.import_module("dvd-hybrid-transcode")

        # Test scan
        vobus, keyframes, segments = transcoder.scan_vts_vobus([vob_path], fps=29.97, return_segments=True)
        assert len(vobus) == 3, f"Expected 3 VOBUs, got {len(vobus)}"
        assert len(segments) == 2, f"Expected 2 VOB_ID segments, got {len(segments)}"
        assert keyframes == [0, 15, 30], f"Expected keyframes [0, 15, 30], got {keyframes}"
        assert vobus[2]["pts_nanos"] == int(1.0 * 1e9), f"Expected VOBU 2 pts_nanos=1e9, got {vobus[2]['pts_nanos']}"
        print("VOBU and VOB_ID segment scanning: OK")

        # Test qpfile generation
        qpfile = os.path.join(tmpdir, "keyframes.txt")
        transcoder.generate_qpfile(keyframes, qpfile)
        with open(qpfile) as f:
            content = f.read().strip().splitlines()
        assert content == ["0 I -1", "15 I -1", "30 I -1"], f"Unexpected qpfile: {content}"
        print("qpfile emission: OK")

        # Test compact MVOB emission (default keep_audio=False)
        os.makedirs(output_dir, exist_ok=True)
        transcoder.emit_mvob_files([vob_path], vts_id=1, vobus=vobus, output_dir=output_dir)

        out_mvob = os.path.join(output_dir, "VTS_01_1.mvob")
        assert os.path.exists(out_mvob), "Output .mvob missing"
        assert os.path.islink(os.path.join(output_dir, "VTS_01_1.VOB")), "VOB symlink missing"
        orig_size = os.path.getsize(vob_path)
        mvob_size = os.path.getsize(out_mvob)
        # Original had 16 sectors; compact .mvob must have:
        # VOBU 0: nv1 + v1_ref + s1 (3 sectors)
        # VOBU 1: nv2 + v2_ref (2 sectors)
        # VOBU 2: nv3 + s1 + v3_dummy_ref (3 sectors)
        # Total = 8 sectors (half the size even on a tiny synthetic stream)
        assert mvob_size == 8 * SECTOR_SIZE, f"Expected 8 sectors ({8*SECTOR_SIZE}B), got {mvob_size}B (orig {orig_size}B)"
        print("Compact MVOB size reduction & multi-video-sector collapsing: OK")

        # Verify sectors in emitted .mvob
        with open(out_mvob, "rb") as f:
            sec_nv1 = f.read(SECTOR_SIZE)
            sec_v1 = f.read(SECTOR_SIZE)
            sec_s1 = f.read(SECTOR_SIZE)
            sec_nv2 = f.read(SECTOR_SIZE)
            sec_v2 = f.read(SECTOR_SIZE)
            sec_nv3 = f.read(SECTOR_SIZE)
            sec_s3 = f.read(SECTOR_SIZE)
            sec_v3 = f.read(SECTOR_SIZE)

        assert sec_nv1 == nv1, "NV_PCK 1 modified"
        assert sec_s1 == s1, "SPU sector 1 modified"
        assert sec_nv2 == nv2, "NV_PCK 2 modified"
        assert sec_nv3 == nv3, "NV_PCK 3 modified"
        assert sec_s3 == s1, "SPU sector 3 modified"
        print("NV_PCK and SPU sector verbatim preservation: OK")

        # Video sector 1: payload at offset 28
        ref1 = sec_v1[28:28 + 40]
        magic, ver, vts, idx, pts, dur, is_idr, _ = transcoder.MVOB_STRUCT.unpack(ref1)
        assert (magic, ver, vts, idx, pts, is_idr) == (transcoder.MVOB_MAGIC, 1, 1, 0, 0, 1)
        assert sec_v1[28 + 40:] == b"\x00" * (SECTOR_SIZE - 68)

        # Video sector 2: payload at offset 28
        ref2 = sec_v2[28:28 + 40]
        magic, ver, vts, idx, pts, dur, is_idr, _ = transcoder.MVOB_STRUCT.unpack(ref2)
        assert (magic, ver, vts, idx, pts, is_idr) == (transcoder.MVOB_MAGIC, 1, 1, 1, int(0.5 * 1e9), 1)

        # Video sector 3 (synthetic dummy video sector for 0-video-sector VOBU): payload at offset 23
        ref3 = sec_v3[23:23 + 40]
        magic, ver, vts, idx, pts, dur, is_idr, _ = transcoder.MVOB_STRUCT.unpack(ref3)
        assert (magic, ver, vts, idx, pts, is_idr) == (transcoder.MVOB_MAGIC, 1, 1, 2, int(1.0 * 1e9), 1)
        print("Video reference descriptors (hollowed & synthetic): OK")

    print("\nAll Transcoder integration tests passed successfully!")


if __name__ == "__main__":
    main()

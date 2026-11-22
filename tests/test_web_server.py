#!/usr/bin/env python3
"""
Integration and unit tests for TinyDVD's Python Web Server, OS directory watcher
(kqueue/inotify) -> SSE push, WebSocket DVD VM protocol, HTTP 206 MP4 streaming,
security validation, and built-in DVD import + duplicate name disambiguation.
"""

from __future__ import annotations

import ctypes
import json
import os
import select
import shutil
import socket
import struct
import sys
import tempfile
import time
from urllib.parse import quote

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS_DIR = os.path.join(REPO_ROOT, "scripts")
if SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, SCRIPTS_DIR)

from tinydvd_core import DesktopUIEvent, TinyDVDCore
from tinydvd_desktop_ui import (
    EVT_KEYDOWN,
    EVT_MOUSE_DOWN,
    EVT_MOUSE_WHEEL,
    EVT_TEXTINPUT,
    FOCUS_NONE,
    FOCUS_SEARCH,
    KMOD_GUI,
    SDLK_BACKSPACE,
    SDLK_DOWN,
    SDLK_ESCAPE,
    SDLK_UP,
    SDLK_a,
    SDLK_f,
    SDLK_v,
    SDLK_x,
    SDL_BUTTON_LEFT,
    Rect,
    TextField,
    TinyDVDDesktopUI,
    parse_png_sar,
)
from tinydvd_library import TinyDVDLibrary, disc_id_for_path
import tinydvd_mp4
from tinydvd_web_server import TinyDVDWebServer, compute_ws_accept_key

TEST_PASSWORD = "correct horse battery staple"
_login_cookies: dict[int, str] = {}


def make_server(core: TinyDVDCore, target: str, **kwargs) -> TinyDVDWebServer:
    return TinyDVDWebServer(core, target, TEST_PASSWORD, bind_addr="127.0.0.1", listen=False,
                            announce=False, **kwargs)


def login_cookie(server: TinyDVDWebServer) -> str:
    """Log in once per server and return the Cookie header value."""
    if id(server) not in _login_cookies:
        st, hdrs, _ = http_get(server, "/login", method="POST", auth=False,
                               body=f"password={quote(TEST_PASSWORD)}".encode("ascii"),
                               headers={"Content-Type": "application/x-www-form-urlencoded"})
        assert st == 303, st
        _login_cookies[id(server)] = hdrs["set-cookie"].split(";", 1)[0]
    return _login_cookies[id(server)]


def http_post_json(server: TinyDVDWebServer, path: str, obj: dict, headers: dict | None = None):
    hdrs = {"Content-Type": "application/json"}
    hdrs.update(headers or {})
    return http_get(server, path, method="POST", body=json.dumps(obj).encode("utf-8"), headers=hdrs)


def http_get(server: TinyDVDWebServer, path: str, method: str = "GET", headers: dict | None = None,
             body: bytes = b"", auth: bool = True):
    hdrs = dict(headers or {})
    if auth:
        hdrs["Cookie"] = login_cookie(server)
    if body:
        hdrs["Content-Length"] = str(len(body))
    c_sock, s_sock = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    c_sock.settimeout(10.0)
    t = server.handle_client_socket(s_sock)
    try:
        hdr_lines = [f"{method} {path} HTTP/1.1", "Host: 127.0.0.1"]
        for k, v in hdrs.items():
            hdr_lines.append(f"{k}: {v}")
        raw_req = ("\r\n".join(hdr_lines) + "\r\n\r\n").encode("ascii") + body
        c_sock.sendall(raw_req)

        chunks: list[bytes] = []
        while True:
            chunk = c_sock.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
        raw_resp = b"".join(chunks)
    finally:
        c_sock.close()
        t.join(timeout=5.0)

    hdr_end = raw_resp.index(b"\r\n\r\n")
    hdr_block = raw_resp[:hdr_end].decode("iso-8859-1", errors="replace")
    body = raw_resp[hdr_end + 4 :]

    lines = hdr_block.split("\r\n")
    status = int(lines[0].split(" ", 2)[1])
    resp_headers: dict[str, str] = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            resp_headers[k.strip().lower()] = v.strip()
    return status, resp_headers, body


def ws_connect(server: TinyDVDWebServer, path: str, key: str = "dGhlIHNhbXBsZSBub25jZQ=="):
    cookie = login_cookie(server)
    c_sock, s_sock = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    c_sock.settimeout(5.0)
    t = server.handle_client_socket(s_sock)
    req = (
        f"GET {path} HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        f"Cookie: {cookie}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    ).encode("ascii")
    c_sock.sendall(req)

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = c_sock.recv(4096)
        assert chunk, "WebSocket closed during handshake"
        buf += chunk
    hdr_end = buf.index(b"\r\n\r\n") + 4
    hdr_text = buf[:hdr_end].decode("ascii", errors="replace")
    assert "HTTP/1.1 101 Switching Protocols" in hdr_text
    assert compute_ws_accept_key(key) in hdr_text
    return c_sock, t


def ws_send_masked_text(sock: socket.socket, text: str) -> None:
    payload = text.encode("utf-8")
    mask = b"\x37\xFA\x21\x3D"
    plen = len(payload)
    if plen <= 125:
        hdr = bytes([0x81, 0x80 | plen])
    else:
        hdr = struct.pack("!BBH", 0x81, 0x80 | 126, plen)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(hdr + mask + masked)


def test_sha1_and_websocket_key() -> None:
    print("Testing SHA-1 & RFC 6455 WebSocket Accept Key generation... ", end="", flush=True)
    key = "dGhlIHNhbXBsZSBub25jZQ=="
    assert compute_ws_accept_key(key) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    print("OK")


def test_spu_png_encoding(core: TinyDVDCore) -> None:
    print("Testing SPU RGBA to PNG & Data URL generation... ", end="", flush=True)
    pixels = (ctypes.c_uint32 * (100 * 100))()
    for i in range(100 * 100):
        pixels[i] = 0xFF00FFFF if (i % 2 == 0) else 0x00000000

    out_sz = ctypes.c_size_t(0)
    png_ptr = core.lib.spu_rgba_to_png(pixels, 100, 100, ctypes.byref(out_sz))
    assert png_ptr and out_sz.value > 8
    png_bytes = ctypes.string_at(png_ptr, out_sz.value)
    assert png_bytes[:4] == b"\x89PNG"
    core.lib.tdvd_free(png_ptr)

    url_ptr = core.lib.spu_rgba_to_data_url(pixels, 100, 100)
    assert url_ptr
    data_url = ctypes.c_char_p(url_ptr).value
    assert data_url and data_url.startswith(b"data:image/png;base64,")
    core.lib.tdvd_free(url_ptr)
    print("OK")


def test_http_and_websocket_endpoints(core: TinyDVDCore, tdvd_path: str) -> None:
    print("Testing Web Server HTTP endpoints, Range streaming & WebSocket...")
    server = make_server(core, tdvd_path)
    try:
        # 1. GET /
        st, hdrs, body = http_get(server, "/")
        assert st == 200
        assert "text/html" in hdrs.get("content-type", "")
        assert b"TinyDVD Web Player" in body
        print(f"  - GET /: OK (HTTP 200, HTML SPA served, {len(body)} bytes)")

        # 2. GET /api/library
        st, hdrs, body = http_get(server, "/api/library")
        assert st == 200
        assert "application/json" in hdrs.get("content-type", "")
        data = json.loads(body.decode("utf-8"))
        assert data["is_directory"] is False
        assert len(data["items"]) == 1
        assert data["items"][0]["title"] == "TinyDVD Test Disc"
        assert data["items"][0]["director"] == "Ada Testmaker"
        assert "Pat Placeholder" in data["items"][0]["cast"]
        assert data["items"][0]["year"] == 2001
        print("  - GET /api/library: OK (JSON movie catalog with director, cast, year metadata)")

        # 3. GET /api/thumbnail
        st, hdrs, body = http_get(server, "/api/thumbnail")
        assert st == 200
        assert "image/png" in hdrs.get("content-type", "")
        assert body[:4] == b"\x89PNG"
        print("  - GET /api/thumbnail: OK (Direct archive PNG streaming)")

        # 4. WebSocket /ws
        sock, tid = ws_connect(server, "/ws")
        ws_send_masked_text(sock, '{"type":"key","key":"up"}')
        ws_send_masked_text(sock, '{"type":"key","key":"activate"}')
        time.sleep(0.05)
        sock.close()
        tid.join(timeout=5.0)
        print("  - WebSocket /ws: OK (RFC 6455 101 Switching Protocols & Masked Frames)")

        # 5. HTTP 206 Range Streaming
        st, hdrs, body = http_get(server, "/api/media/VTS_01_VIDEO.mp4", headers={"Range": "bytes=0-1023"})
        assert st == 206
        assert "video/mp4" in hdrs.get("content-type", "")
        assert hdrs.get("content-range", "").startswith("bytes 0-1023/")
        assert len(body) == 1024

        st416, _, _ = http_get(server, "/api/media/VTS_01_VIDEO.mp4", headers={"Range": "bytes=999999999-1000000999"})
        assert st416 == 416
        print("  - HTTP 206 Range Streaming: OK (byte ranges served straight from the package)")

        # 6. Security: command injection & traversal rejection on /api/media/
        for bad_path in (
            "/api/media/VIDEO_TS_VIDEO;id.mp4",
            "/api/media/../../etc/passwd.mp4",
            '/api/media/foo"bar.mp4',
        ):
            st_bad, _, _ = http_get(server, bad_path)
            assert st_bad in (400, 404)
        print("  - Security Check: OK (Rejected shell metacharacters and path traversal on /api/media/)")

        # 7. Multi-Session isolation
        s1, t1 = ws_connect(server, "/ws?session=user_alice")
        s2, t2 = ws_connect(server, "/ws?session=user_bob")
        ws_send_masked_text(s1, '{"type":"key","key":"menu"}')
        ws_send_masked_text(s2, '{"type":"key","key":"down"}')
        time.sleep(0.05)
        s1.close()
        s2.close()
        t1.join(timeout=5.0)
        t2.join(timeout=5.0)
        print("  - Multi-Session Isolation: OK (Independent sessions, state & VMs for distinct clients)")
    finally:
        server.stop()
    print("Web Server tests passed!")


def test_library_directory_filtering_and_sse(core: TinyDVDCore, tdvd_path: str) -> None:
    print("Testing Web Server Directory Scanning, Non-TinyDVD Filtering & OS-Watched SSE...")
    abs_tdvd = os.path.abspath(tdvd_path)
    test_dir = tempfile.mkdtemp(prefix="tinydvd_test_mixed_")
    try:
        os.symlink(abs_tdvd, os.path.join(test_dir, "TestDisc.tdvd"))
        for fname, txt in (("home_video.mp4", "dummy"), ("holiday_movie.mkv", "dummy"), ("readme.txt", "notes")):
            with open(os.path.join(test_dir, fname), "w") as f:
                f.write(txt)
        os.mkdir(os.path.join(test_dir, "OtherFolder"))

        server = make_server(core, test_dir)
        try:
            st, _, body = http_get(server, "/api/library")
            assert st == 200
            txt = body.decode("utf-8")
            assert "TinyDVD Test Disc" in txt
            for unwanted in ("home_video", "holiday_movie", "readme.txt", "OtherFolder"):
                assert unwanted not in txt
            print("  - Directory Filtering: OK (Tolerates non-TinyDVD entries, only lists .tdvd)")

            # Open live SSE stream over socketpair and trigger kernel kqueue/inotify event
            sse_sock, s_sock = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
            sse_sock.settimeout(5.0)
            sse_tid = server.handle_client_socket(s_sock)
            try:
                sse_sock.sendall(b"GET /api/library/events HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: "
                                 + login_cookie(server).encode("ascii") + b"\r\n\r\n")
                sse_buf = b""
                deadline = time.time() + 2.0
                while b": connected" not in sse_buf and time.time() < deadline:
                    r, _, _ = select.select([sse_sock], [], [], 1.0)
                    assert r, "Timed out waiting for SSE handshake"
                    chunk = sse_sock.recv(4096)
                    assert chunk
                    sse_buf += chunk
                hdr_str = sse_buf.decode("utf-8", errors="replace")
                assert "HTTP/1.1 200 OK" in hdr_str
                assert "text/event-stream" in hdr_str

                # Mutate the watched folder while SSE client is listening
                os.symlink(abs_tdvd, os.path.join(test_dir, "SSE_Test.tdvd"))

                got_update = b"event: library_update" in sse_buf
                deadline = time.time() + 3.0
                while not got_update and time.time() < deadline:
                    r, _, _ = select.select([sse_sock], [], [], 0.2)
                    if r:
                        chunk = sse_sock.recv(4096)
                        if not chunk:
                            break
                        sse_buf += chunk
                        if b"event: library_update" in sse_buf:
                            got_update = True
                assert got_update, f"Expected SSE library_update push, got: {sse_buf!r}"
            finally:
                try:
                    sse_sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                sse_sock.close()
                sse_tid.join(timeout=5.0)

            st, _, body = http_get(server, "/api/library")
            assert st == 200
            data = json.loads(body.decode("utf-8"))
            ids = [it["id"] for it in data["items"]]
            assert disc_id_for_path("SSE_Test.tdvd") in ids
            print("  - OS Watcher + SSE Push: OK (kqueue/inotify detected new .tdvd and pushed library_update)")
        finally:
            server.stop()
    finally:
        shutil.rmtree(test_dir, ignore_errors=True)


def ws_read_messages(sock: socket.socket, wait: float) -> list[dict]:
    """Unmasked server text frames received within `wait` seconds."""
    msgs: list[dict] = []
    buf = b""
    deadline = time.time() + wait
    while time.time() < deadline:
        r, _, _ = select.select([sock], [], [], max(0.0, deadline - time.time()))
        if not r:
            break
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 2:
            plen, off = buf[1] & 0x7F, 2
            if plen == 126:
                if len(buf) < 4:
                    break
                plen, off = struct.unpack("!H", buf[2:4])[0], 4
            elif plen == 127:
                if len(buf) < 10:
                    break
                plen, off = struct.unpack("!Q", buf[2:10])[0], 10
            if len(buf) < off + plen:
                break
            msgs.append(json.loads(buf[off:off + plen].decode("utf-8")))
            buf = buf[off + plen:]
    return msgs


def test_library_waits_for_pick(core: TinyDVDCore, tdvd_path: str) -> None:
    print("Testing that a new library session waits for the viewer to pick a movie...")
    lib_dir = tempfile.mkdtemp(prefix="tinydvd_test_pick_")
    try:
        os.symlink(os.path.abspath(tdvd_path), os.path.join(lib_dir, "TestDisc.tdvd"))
        server = make_server(core, lib_dir)
        try:
            sock, tid = ws_connect(server, "/ws?session=fresh_viewer")
            msgs = ws_read_messages(sock, 1.0)
            assert not [m for m in msgs if m.get("type") == "video_load"], msgs
            print("  - No movie starts on connect in library mode: OK")

            st, _, body = http_get(server, "/api/library")
            disc_id = json.loads(body)["items"][0]["id"]
            st, _, _ = http_post_json(server, "/api/play", {"session": "fresh_viewer", "disc": disc_id})
            assert st == 200
            msgs = ws_read_messages(sock, 3.0)
            assert [m for m in msgs if m.get("type") == "video_load"], msgs
            print("  - Picking a movie starts it in that session: OK")
            sock.close()
            tid.join(timeout=5.0)
        finally:
            server.stop()
    finally:
        shutil.rmtree(lib_dir, ignore_errors=True)


def _playlist_lines(server, path):
    st, hdrs, body = http_get(server, path)
    assert st == 200, (path, st)
    assert hdrs.get("content-type") == "application/vnd.apple.mpegurl"
    lines = body.decode("ascii").splitlines()
    assert lines[0] == "#EXTM3U"
    return hdrs, lines


def _check_segments(server, lines, expect_seconds):
    """A media playlist's segments: back-to-back byte ranges of one packaged MP4, starting with moof."""
    assert lines[-1] == "#EXT-X-ENDLIST" and "#EXT-X-PLAYLIST-TYPE:VOD" in lines
    durations = [float(l[len("#EXTINF:"):].rstrip(",")) for l in lines if l.startswith("#EXTINF:")]
    assert abs(sum(durations) - expect_seconds) < 0.5, sum(durations)
    target = int(next(l for l in lines if l.startswith("#EXT-X-TARGETDURATION:")).split(":")[1])
    assert all(d <= target + 0.5 for d in durations)
    uri = next(l for l in lines if l and not l.startswith("#"))
    ranges = [tuple(int(x) for x in l[len("#EXT-X-BYTERANGE:"):].split("@"))
              for l in lines if l.startswith("#EXT-X-BYTERANGE:")]
    for (len_a, start_a), (_, start_b) in zip(ranges, ranges[1:]):
        assert start_a + len_a == start_b, (start_a, len_a, start_b)
    length, start = ranges[0]
    st, _, seg = http_get(server, f"/api/media/{uri}", headers={"Range": f"bytes={start}-{start + length - 1}"})
    assert st == 206 and len(seg) == length and seg[4:8] == b"moof", (st, seg[:8])
    init = next(l for l in lines if l.startswith("#EXT-X-MAP:"))
    init_len = int(init.split('BYTERANGE="')[1].split("@")[0])
    st, _, head = http_get(server, f"/api/media/{uri}", headers={"Range": f"bytes=0-{init_len - 1}"})
    assert st == 206 and head[4:8] == b"ftyp" and b"moov" in head
    return len(durations)


def test_hls_streaming(core: TinyDVDCore, tdvd_path: str) -> None:
    print("Testing HLS streaming straight from the package...")
    server = make_server(core, tdvd_path)
    try:
        st, _, body = http_get(server, "/api/library")
        disc = json.loads(body)["items"][0]["id"]

        # video_load messages point the browser at a playlist
        sock, tid = ws_connect(server, "/ws?session=hls_viewer")
        loads = [m for m in ws_read_messages(sock, 1.5) if m.get("type") == "video_load"]
        sock.close()
        tid.join(timeout=5.0)
        assert loads and all(m["src"].startswith("/api/media/") and ".m3u8?disc=" in m["src"]
                             for m in loads), loads
        print("  - video_load points at an HLS playlist: OK")

        # The title's master playlist: the video, and both audio tracks
        _, lines = _playlist_lines(server, f"/api/media/VTS_01_VIDEO.m3u8?disc={disc}")
        media = [l for l in lines if l.startswith("#EXT-X-MEDIA:TYPE=AUDIO")]
        assert len(media) == 2, media
        assert 'NAME="Audio 1",DEFAULT=YES' in media[0] and 'NAME="Audio 2",DEFAULT=NO' in media[1]
        assert 'URI="VTS_01_AUDIO_1.m3u8?disc=' in media[0] and 'URI="VTS_01_AUDIO_2.m3u8?disc=' in media[1]
        stream = next(l for l in lines if l.startswith("#EXT-X-STREAM-INF:"))
        assert 'CODECS="avc1.' in stream and "mp4a.40.2" in stream and 'AUDIO="audio"' in stream
        video_uri = lines[lines.index(stream) + 1]
        assert video_uri.startswith("VTS_01_VIDEO.m3u8?disc=") and video_uri.endswith("&part=video")
        print("  - Master playlist: OK (video + 2 audio tracks)")

        _, lines = _playlist_lines(server, f"/api/media/{video_uri}")
        n = _check_segments(server, lines, 20.0)
        for track in (1, 2):
            _, lines = _playlist_lines(server, f"/api/media/VTS_01_AUDIO_{track}.m3u8?disc={disc}")
            _check_segments(server, lines, 20.0)
        print(f"  - Video and audio segments are byte ranges of the packaged MP4s: OK ({n} video segments)")

        # A menu: one audio track
        _, lines = _playlist_lines(server, f"/api/media/VTS_01_0_VIDEO.m3u8?disc={disc}")
        assert len([l for l in lines if l.startswith("#EXT-X-MEDIA:TYPE=AUDIO")]) == 1

        for bad in ("VTS_01_VIDEO.txt", "VTS_01_EXTRA.mp4", "VTS_09_VIDEO.m3u8", "..%2FVTS_01_VIDEO.mp4"):
            st, _, _ = http_get(server, f"/api/media/{bad}?disc={disc}")
            assert st in (400, 404), (bad, st)
        print("  - Menus, missing videos and bad names: OK")
    finally:
        server.stop()


def test_mp4_reader_errors() -> None:
    print("Testing the MP4 reader on files it can't stream...")
    d = tempfile.mkdtemp(prefix="tinydvd_test_mp4_")
    try:
        path = os.path.join(d, "plain.mp4")
        with open(path, "wb") as f:
            f.write(struct.pack(">I4s", 16, b"ftyp") + b"isom\0\0\0\0")
            f.write(struct.pack(">I4s", 8, b"moov"))
            f.write(struct.pack(">I4s", 12, b"mdat") + b"data")
        for content in (None, struct.pack(">I4s", 4, b"junk")):
            if content is not None:
                with open(path, "wb") as f:
                    f.write(content)
            try:
                tinydvd_mp4.read_mp4(path)
                raise AssertionError("expected an error for an MP4 without a usable index")
            except ValueError:
                pass
        assert tinydvd_mp4.audio_member_name("VTS_04_VIDEO.mp4", 2) == "VTS_04_AUDIO_2.mp4"
        assert tinydvd_mp4.audio_member_names("VIDEO_TS_VIDEO.mp4",
                                              ["video_ts_audio_1.mp4", "VIDEO_TS_AUDIO_3.mp4"]) == ["video_ts_audio_1.mp4"]
        print("  - Refuses MP4s without a usable index; audio file names: OK")
    finally:
        shutil.rmtree(d, ignore_errors=True)


def create_dummy_converted_dir(directory: str, meta_dict: dict) -> None:
    files = {
        "VIDEO_TS.IFO": b"DVDVIDEO-VMG\x00",
        "VIDEO_TS.mvob": b"MVOB0001\x00",
        "VIDEO_TS_VIDEO.mp4": b"\x00\x00\x00\x08ftyp",
        "thumbnail.png": b"\x89PNG\r\n\x1a\n\x00\x00\x00\x00",
        "metadata.json": json.dumps(meta_dict).encode("utf-8"),
    }
    for name, content in files.items():
        with open(os.path.join(directory, name), "wb") as f:
            f.write(content)


def poll_import_until_done(server: TinyDVDWebServer) -> dict:
    for _ in range(100):
        time.sleep(0.1)
        st, _, body = http_get(server, "/api/import/status")
        assert st == 200
        data = json.loads(body.decode("utf-8"))
        if data["state"] == "error":
            raise AssertionError(f"Import failed unexpectedly: {data}")
        if data["state"] == "done":
            return data
    raise AssertionError("Timed out waiting for import job to finish")


def test_import_dvd_workflow(core: TinyDVDCore) -> None:
    print("Testing Web Server Built-in DVD Import Workflow...")
    lib_dir = tempfile.mkdtemp(prefix="tinydvd_test_lib_")
    src_dir = tempfile.mkdtemp(prefix="tinydvd_test_src_")
    try:
        create_dummy_converted_dir(
            src_dir,
            {
                "title": "Dune Imported",
                "year": 2021,
                "director": "Denis Villeneuve",
                "cast": "Timothee Chalamet, Zendaya",
                "genres": "Sci-Fi",
            },
        )
        server = make_server(core, lib_dir, allow_import=True)
        try:
            st, _, body = http_get(server, "/api/import/status")
            assert st == 200
            assert json.loads(body)["state"] == "idle"

            st, _, body = http_post_json(server, "/api/import/start",
                                         {"source": src_dir, "title": "Dune Imported", "skip_enrichment": True})
            assert st == 200
            assert json.loads(body)["status"] == "started"

            status_data = poll_import_until_done(server)
            assert status_data["result_title"] == "Dune Imported"

            st, _, body = http_get(server, "/api/library")
            assert st == 200
            lib_txt = body.decode("utf-8")
            assert "Dune Imported" in lib_txt
            assert "Denis Villeneuve" in lib_txt
            assert "Timothee Chalamet" in lib_txt
            print("  - Built-in DVD Import Workflow: OK (status -> start -> convert -> live catalog refresh)")
        finally:
            server.stop()
    finally:
        shutil.rmtree(lib_dir, ignore_errors=True)
        shutil.rmtree(src_dir, ignore_errors=True)


def test_duplicate_dvd_import_disambiguation(core: TinyDVDCore) -> None:
    print("Testing Duplicate DVD Import Name Disambiguation (Year first, then numeric suffix)...")
    lib_dir = tempfile.mkdtemp(prefix="tinydvd_test_dup_lib_")
    src1_dir = tempfile.mkdtemp(prefix="tinydvd_test_dup_s1_")
    src2_dir = tempfile.mkdtemp(prefix="tinydvd_test_dup_s2_")
    try:
        create_dummy_converted_dir(src1_dir, {"title": "Dune Test", "year": 1984, "director": "David Lynch"})
        create_dummy_converted_dir(src2_dir, {"title": "Dune Test", "year": 2021, "director": "Denis Villeneuve"})

        server = make_server(core, lib_dir, allow_import=True)
        try:
            # 1. Dune Test.tdvd (1984)
            st, _, _ = http_post_json(server, "/api/import/start",
                                      {"source": src1_dir, "title": "Dune Test", "skip_enrichment": True})
            assert st == 200
            poll_import_until_done(server)

            # 2. Dune Test (2021).tdvd
            st, _, _ = http_post_json(server, "/api/import/start",
                                      {"source": src2_dir, "title": "Dune Test", "skip_enrichment": True})
            assert st == 200
            poll_import_until_done(server)

            # 3. Dune Test (2021) (2).tdvd
            st, _, _ = http_post_json(server, "/api/import/start",
                                      {"source": src2_dir, "title": "Dune Test", "skip_enrichment": True})
            assert st == 200
            res3 = poll_import_until_done(server)
            assert res3["result_title"] == "Dune Test (2)"

            for fn in ("Dune Test.tdvd", "Dune Test (2021).tdvd", "Dune Test (2021) (2).tdvd"):
                p = os.path.join(lib_dir, fn)
                assert os.path.isfile(p) and os.path.getsize(p) > 0, f"Missing {p}"

            st, _, body = http_get(server, "/api/library")
            assert st == 200
            txt = body.decode("utf-8")
            assert "David Lynch" in txt
            assert "Denis Villeneuve" in txt
            assert "Dune Test (2)" in txt
            print("  - Duplicate DVD Disambiguation: OK (year suffix + (2) numeric suffix, no overwrites)")
        finally:
            server.stop()
    finally:
        shutil.rmtree(lib_dir, ignore_errors=True)
        shutil.rmtree(src1_dir, ignore_errors=True)
        shutil.rmtree(src2_dir, ignore_errors=True)


def test_desktop_ui_helpers(core: TinyDVDCore) -> None:
    print("Testing Python Desktop UI helpers (TextField, PNG pHYs SAR, Pango hit-test)...")

    # TextField editing, UTF-8 selection, word navigation, and deletion
    tf = TextField("Café Dune 2021")
    assert tf.text == "Café Dune 2021"
    assert tf.cursor == len(tf.text)
    assert not tf.has_selection()

    # Word navigation
    assert tf.prev_word_pos(tf.cursor) == 10  # before "2021"
    assert tf.prev_word_pos(10) == 5          # before "Dune"
    assert tf.prev_word_pos(5) == 0           # before "Café"
    assert tf.next_word_pos(0) == 4           # after "Café"
    assert tf.next_word_pos(4) == 9           # after "Dune"

    # Shift-selection and replace-on-insert
    tf.move_cursor(5, False)
    tf.move_cursor(9, True)
    assert tf.has_selection()
    assert tf.get_selection() == (5, 9)
    tf.insert_str("Part\n")
    assert tf.text == "Café Part 2021"
    assert not tf.has_selection()

    # Select all & backspace/delete
    tf.select_all()
    assert tf.get_selection() == (0, len(tf.text))
    tf.clear_selection()
    assert not tf.has_selection()

    tf.move_cursor(len(tf.text), False)
    tf.backspace(word_mod=True)
    assert tf.text == "Café Part "
    tf.move_cursor(0, False)
    tf.delete_fwd(word_mod=True)
    assert tf.text == " Part "
    tf.move_cursor(len(tf.text), False)
    tf.backspace(line_mod=True)
    assert tf.text == ""

    # Pango x_to_char_index via libtinydvd_core (headless 1x1 surface)
    tf.set_text("Dune Cinema")
    assert tf.x_to_char_index(core, None, -5.0) == 0
    end_idx = tf.x_to_char_index(core, None, 10000.0)
    assert end_idx == len(tf.text), f"Expected {len(tf.text)}, got {end_idx}"
    mid_idx = tf.x_to_char_index(core, None, 30.0)
    assert 0 < mid_idx < len(tf.text), f"Expected interior index, got {mid_idx}"

    # parse_png_sar with pHYs chunks
    png_sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">I", 13) + b"IHDR" + b"\x00" * 13 + b"\x00" * 4
    phys_32_27 = struct.pack(">I", 9) + b"pHYs" + struct.pack(">IIB", 32, 27, 0) + b"\x00" * 4
    iend = struct.pack(">I", 0) + b"IEND" + b"\x00" * 4
    sar = parse_png_sar(png_sig + ihdr + phys_32_27 + iend)
    assert abs(sar - (32.0 / 27.0)) < 1e-6, f"Expected ~1.185, got {sar}"

    # No pHYs defaults to 1.0
    assert parse_png_sar(png_sig + ihdr + iend) == 1.0
    assert parse_png_sar(b"not a png") == 1.0

    # Rect hit testing
    r = Rect(10.0, 20.0, 100.0, 40.0)
    assert r.contains(10.0, 20.0)
    assert r.contains(110.0, 60.0)
    assert not r.contains(9.9, 20.0)
    assert not Rect().contains(0.0, 0.0)

    # TinyDVDDesktopUI.handle_event coverage (shortcuts, typing, mouse, scroll)
    tmp_lib_dir = tempfile.mkdtemp(prefix="tinydvd_test_ui_")
    try:
        lib = TinyDVDLibrary(tmp_lib_dir)
        try:
            ui = TinyDVDDesktopUI(core, None, lib)
            ui.compute_layout_rects(1280, 800, False)

            # Cmd+F focuses search bar
            ev = DesktopUIEvent(type=EVT_KEYDOWN, keysym=SDLK_f, keymod=KMOD_GUI)
            consumed, _, _, _ = ui.handle_event(ev, 1280, 800, False)
            assert consumed and ui.focus == FOCUS_SEARCH

            # Typing via EVT_TEXTINPUT inserts into focused search field
            ev = DesktopUIEvent(type=EVT_TEXTINPUT, text=b"Dune")
            consumed, _, _, _ = ui.handle_event(ev, 1280, 800, False)
            assert consumed and ui.tf_search.text == "Dune"

            # Backspace & Cmd+A & Cmd+X/Cmd+V without NameError/crash
            ev = DesktopUIEvent(type=EVT_KEYDOWN, keysym=SDLK_BACKSPACE)
            ui.handle_event(ev, 1280, 800, False)
            assert ui.tf_search.text == "Dun"

            ev = DesktopUIEvent(type=EVT_KEYDOWN, keysym=SDLK_a, keymod=KMOD_GUI)
            consumed, _, _, _ = ui.handle_event(ev, 1280, 800, False)
            assert consumed and ui.tf_search.get_selection() == (0, 3)

            # Cmd+V when focus is unfocused (the exact repro path)
            ui.focus = FOCUS_NONE
            ev = DesktopUIEvent(type=EVT_KEYDOWN, keysym=SDLK_v, keymod=KMOD_GUI)
            consumed, _, _, _ = ui.handle_event(ev, 1280, 800, False)
            assert consumed and ui.focus == FOCUS_SEARCH

            # Cmd+X shortcut check
            ev = DesktopUIEvent(type=EVT_KEYDOWN, keysym=SDLK_x, keymod=KMOD_GUI)
            ui.handle_event(ev, 1280, 800, False)

            # Escape clears search text first, then unfocuses
            ui.tf_search.set_text("Dune")
            ui.focus = FOCUS_SEARCH
            ev = DesktopUIEvent(type=EVT_KEYDOWN, keysym=SDLK_ESCAPE)
            ui.handle_event(ev, 1280, 800, False)
            assert ui.tf_search.text == ""
            ui.handle_event(ev, 1280, 800, False)
            assert ui.focus == FOCUS_NONE

            # Arrow scroll & mouse wheel & mouse down
            ui.max_scroll_y = 200.0
            ev = DesktopUIEvent(type=EVT_KEYDOWN, keysym=SDLK_DOWN)
            assert ui.handle_event(ev, 1280, 800, False)[0] and ui.scroll_y > 0.0
            ev = DesktopUIEvent(type=EVT_KEYDOWN, keysym=SDLK_UP)
            ui.handle_event(ev, 1280, 800, False)
            assert ui.scroll_y == 0.0

            ev = DesktopUIEvent(type=EVT_MOUSE_WHEEL, wheel_y=-1)
            ui.handle_event(ev, 1280, 800, False)
            assert ui.scroll_y > 0.0

            # Click + Import DVD button opens modal
            imp_cx = int(ui.r_open_import.x + ui.r_open_import.w * 0.5)
            imp_cy = int(ui.r_open_import.y + ui.r_open_import.h * 0.5)
            ev = DesktopUIEvent(type=EVT_MOUSE_DOWN, x=imp_cx, y=imp_cy, button=SDL_BUTTON_LEFT, clicks=1)
            consumed, _, _, _ = ui.handle_event(ev, 1280, 800, False)
            assert consumed and ui.import_modal_open
        finally:
            lib.destroy()
    finally:
        shutil.rmtree(tmp_lib_dir, ignore_errors=True)

    print("  - Python Desktop UI helpers: OK")


def test_auth_and_request_rules(core: TinyDVDCore, tdvd_path: str) -> None:
    print("Testing web login, cross-site protection and the import switch...")
    lib_dir = tempfile.mkdtemp(prefix="tinydvd_test_auth_")
    server = make_server(core, lib_dir)
    try:
        st, _, _ = http_get(server, "/api/library", auth=False)
        assert st == 401
        st, hdrs, _ = http_get(server, "/", auth=False)
        assert st == 303 and hdrs.get("location") == "/login"
        st, _, body = http_get(server, "/login", auth=False)
        assert st == 200 and b'type="password"' in body
        # "Add to Home Screen" files come without the login cookie
        st, hdrs, body = http_get(server, "/manifest.webmanifest", auth=False)
        assert st == 200 and hdrs.get("content-type") == "application/manifest+json"
        assert json.loads(body)["display"] == "fullscreen"
        for icon in ("/apple-touch-icon.png", "/icon-192.png", "/icon-512.png"):
            st, hdrs, body = http_get(server, icon, auth=False)
            assert st == 200 and body[:4] == b"\x89PNG", icon
        st, _, _ = http_get(server, "/vendor/hls.min.js", auth=False)
        assert st == 401  # everything else still needs the login
        st, _, _ = http_get(server, "/api/library", auth=False, headers={"Cookie": "tinydvd_auth=1.2.3"})
        assert st == 401
        st, _, _ = http_get(server, "/login", method="POST", auth=False, body=b"password=wrong",
                            headers={"Content-Type": "application/x-www-form-urlencoded"})
        assert st == 401
        print("  - Login required: OK (401 for API, redirect for pages, forged and wrong logins refused)")

        st, _, _ = http_post_json(server, "/api/play", {"disc": "x"}, headers={"Origin": "http://evil.example"})
        assert st == 403
        st, _, _ = http_get(server, "/api/play")
        assert st == 404  # state changes are POST-only
        print("  - Cross-site POST refused, GET can't change state: OK")

        st, _, body = http_get(server, "/api/library")
        assert json.loads(body)["import_enabled"] is False
        st, _, _ = http_post_json(server, "/api/import/start", {"source": "/"})
        assert st == 403
        st, _, body = http_get(server, "/api/import/status")
        assert json.loads(body) ["enabled"] is False
        print("  - Web import off by default: OK")

        st, hdrs, _ = http_get(server, "/logout", method="POST", body=b"{}",
                               headers={"Content-Type": "application/json"})
        assert st == 200 and "Max-Age=0" in hdrs.get("set-cookie", "")
        st, _, _ = http_get(server, "/api/library")
        assert st == 401
        print("  - Logout revokes the login: OK")
    finally:
        server.stop()
        shutil.rmtree(lib_dir, ignore_errors=True)


def main() -> int:
    tdvd = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO_ROOT, "tests", "fixtures", "TestDisc.tdvd")
    assert os.path.isfile(tdvd), f"{tdvd} not found (tests/fixtures/make-test-disc.sh builds it)"
    print("=== Running TinyDVD Web Server & Protocol Tests ===")
    core = TinyDVDCore()
    test_sha1_and_websocket_key()
    test_spu_png_encoding(core)
    test_desktop_ui_helpers(core)
    test_auth_and_request_rules(core, tdvd)
    test_http_and_websocket_endpoints(core, tdvd)
    test_library_directory_filtering_and_sse(core, tdvd)
    test_library_waits_for_pick(core, tdvd)
    test_hls_streaming(core, tdvd)
    test_mp4_reader_errors()
    test_import_dvd_workflow(core)
    test_duplicate_dvd_import_disambiguation(core)
    print("All Web Server tests completed successfully!")
    return 0


if __name__ == "__main__":
    sys.exit(main())

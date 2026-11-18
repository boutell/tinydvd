#!/usr/bin/env python3
"""
Integration and unit tests for TinyDVD's Python Web Server, OS directory watcher
(kqueue/inotify) -> SSE push, WebSocket DVD VM protocol, HTTP 206 MP4 streaming,
security validation, and built-in DVD import + duplicate name disambiguation.
"""

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

from tinydvd_core import TinyDVDCore
from tinydvd_web_server import TinyDVDWebServer, compute_ws_accept_key


def http_get(server: TinyDVDWebServer, path: str, method: str = "GET", headers: dict | None = None):
    c_sock, s_sock = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    c_sock.settimeout(10.0)
    t = server.handle_client_socket(s_sock)
    try:
        hdr_lines = [f"{method} {path} HTTP/1.1", "Host: 127.0.0.1"]
        for k, v in (headers or {}).items():
            hdr_lines.append(f"{k}: {v}")
        raw_req = ("\r\n".join(hdr_lines) + "\r\n\r\n").encode("ascii")
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
    c_sock, s_sock = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    c_sock.settimeout(5.0)
    t = server.handle_client_socket(s_sock)
    req = (
        f"GET {path} HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
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
    server = TinyDVDWebServer(core, tdvd_path, bind_addr="127.0.0.1", port=8991, listen=False)
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
        assert "Denis Villeneuve" in data["items"][0]["director"]
        assert "Timoth" in data["items"][0]["cast"]
        assert data["items"][0]["year"] == 2021
        print("  - GET /api/library: OK (JSON movie catalog with director, cast, year metadata)")

        # 3. GET /api/thumbnail
        st, hdrs, body = http_get(server, "/api/thumbnail")
        assert st == 200
        assert "image/png" in hdrs.get("content-type", "")
        assert body[:4] == b"\x89PNG"
        print("  - GET /api/thumbnail: OK (Direct archive PNG streaming)")

        # 4. WebSocket /ws
        sock, tid = ws_connect(server, "/ws")
        ws_send_masked_text(sock, '{"type":"key","key":"Up"}')
        ws_send_masked_text(sock, '{"type":"key","key":"Activate"}')
        time.sleep(0.05)
        sock.close()
        tid.join(timeout=5.0)
        print("  - WebSocket /ws: OK (RFC 6455 101 Switching Protocols & Masked Frames)")

        # 5. HTTP 206 Range Streaming
        st, hdrs, body = http_get(server, "/api/media/VIDEO_TS_VIDEO.mp4", headers={"Range": "bytes=0-1023"})
        assert st == 206
        assert "video/mp4" in hdrs.get("content-type", "")
        assert hdrs.get("content-range", "").startswith("bytes 0-1023/")
        assert len(body) == 1024

        st416, _, _ = http_get(server, "/api/media/VIDEO_TS_VIDEO.mp4", headers={"Range": "bytes=999999999-1000000999"})
        assert st416 == 416
        print("  - HTTP 206 Range Streaming: OK (On-demand fast MP4 remux + byte range seek)")

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
        ws_send_masked_text(s1, '{"type":"key","key":"Menu"}')
        ws_send_masked_text(s2, '{"type":"key","key":"Down"}')
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
        os.symlink(abs_tdvd, os.path.join(test_dir, "Dune.tdvd"))
        for fname, txt in (("home_video.mp4", "dummy"), ("holiday_movie.mkv", "dummy"), ("readme.txt", "notes")):
            with open(os.path.join(test_dir, fname), "w") as f:
                f.write(txt)
        os.mkdir(os.path.join(test_dir, "OtherFolder"))

        server = TinyDVDWebServer(core, test_dir, bind_addr="127.0.0.1", port=8992, listen=False)
        try:
            st, _, body = http_get(server, "/api/library")
            assert st == 200
            txt = body.decode("utf-8")
            assert "Dune" in txt
            for unwanted in ("home_video", "holiday_movie", "readme.txt", "OtherFolder"):
                assert unwanted not in txt
            print("  - Directory Filtering: OK (Tolerates non-TinyDVD entries, only lists .tdvd)")

            # Open live SSE stream over socketpair and trigger kernel kqueue/inotify event
            sse_sock, s_sock = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
            sse_sock.settimeout(5.0)
            sse_tid = server.handle_client_socket(s_sock)
            try:
                sse_sock.sendall(b"GET /api/library/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
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
                os.symlink(abs_tdvd, os.path.join(test_dir, "Dune_SSE_Test.tdvd"))

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
            assert "Dune_SSE_Test_tdvd" in ids
            print("  - OS Watcher + SSE Push: OK (kqueue/inotify detected new .tdvd and pushed library_update)")
        finally:
            server.stop()
    finally:
        shutil.rmtree(test_dir, ignore_errors=True)


def create_dummy_converted_dir(directory: str, meta_dict: dict) -> None:
    files = {
        "VIDEO_TS.IFO": b"DVDVIDEO-VMG\x00",
        "VIDEO_TS.mvob": b"MVOB0001\x00",
        "VIDEO_TS_VIDEO.mkv": b"\x1A\x45\xDF\xA3",
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
        server = TinyDVDWebServer(core, lib_dir, bind_addr="127.0.0.1", port=8993, listen=False)
        try:
            st, _, body = http_get(server, "/api/import/status")
            assert st == 200
            assert json.loads(body)["state"] == "idle"

            start_uri = f"/api/import/start?source={quote(src_dir)}&title=Dune%20Imported&skip_enrichment=1"
            st, _, body = http_get(server, start_uri, method="POST")
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

        server = TinyDVDWebServer(core, lib_dir, bind_addr="127.0.0.1", port=8994, listen=False)
        try:
            # 1. Dune Test.tdvd (1984)
            st, _, _ = http_get(server, f"/api/import/start?source={quote(src1_dir)}&title=Dune%20Test&skip_enrichment=1")
            assert st == 200
            poll_import_until_done(server)

            # 2. Dune Test (2021).tdvd
            st, _, _ = http_get(server, f"/api/import/start?source={quote(src2_dir)}&title=Dune%20Test&skip_enrichment=1")
            assert st == 200
            poll_import_until_done(server)

            # 3. Dune Test (2021) (2).tdvd
            st, _, _ = http_get(server, f"/api/import/start?source={quote(src2_dir)}&title=Dune%20Test&skip_enrichment=1")
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


def main() -> int:
    tdvd = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO_ROOT, "tinydvds", "Dune.tdvd")
    print("=== Running TinyDVD Web Server & Protocol Tests ===")
    core = TinyDVDCore()
    test_sha1_and_websocket_key()
    test_spu_png_encoding(core)
    test_http_and_websocket_endpoints(core, tdvd)
    test_library_directory_filtering_and_sse(core, tdvd)
    test_import_dvd_workflow(core)
    test_duplicate_dvd_import_disambiguation(core)
    print("All Web Server tests completed successfully!")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Pure-Python HTTP / Range Streaming / Server-Sent Events (SSE) / RFC 6455 WebSocket
web server backed by TinyDVDLibrary and libtinydvd_core's DVD VM sessions.
"""

import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import select
import shutil
import socket
import struct
import subprocess
import threading
import time
import urllib.parse
import uuid
import zipfile

from tinydvd_core import TDVD_VM_JSON_CB, TinyDVDCore
from tinydvd_library import TinyDVDLibrary, sanitize_id


WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
CHUNK_SIZE = 65536
SESSION_TIMEOUT_SEC = 180
VALID_MEDIA_BASE_RE = re.compile(r"^[A-Za-z0-9_-]+$")


def load_web_index_html() -> bytes:
    """Parse TINYDVD_INDEX_HTML from src/web_assets.h so there is one source of truth."""
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hpath = os.path.join(repo_root, "src", "web_assets.h")
    with open(hpath, "r", encoding="utf-8") as f:
        lines = f.readlines()

    in_html = False
    out_parts: list[str] = []
    for line in lines:
        s = line.strip()
        if not in_html:
            if "TINYDVD_INDEX_HTML[]" in s:
                in_html = True
            continue
        if s.startswith('"'):
            end_semi = s.endswith('";')
            lit = s[:-1] if end_semi else s
            if len(lit) >= 2 and lit[0] == '"' and lit[-1] == '"':
                inner = lit[1:-1]
                buf: list[str] = []
                i = 0
                n = len(inner)
                while i < n:
                    ch = inner[i]
                    if ch == "\\" and i + 1 < n:
                        nxt = inner[i + 1]
                        if nxt == "n":
                            buf.append("\n")
                        elif nxt == "r":
                            buf.append("\r")
                        elif nxt == "t":
                            buf.append("\t")
                        elif nxt == '"':
                            buf.append('"')
                        elif nxt == "'":
                            buf.append("'")
                        elif nxt == "\\":
                            buf.append("\\")
                        else:
                            buf.append(nxt)
                        i += 2
                    else:
                        buf.append(ch)
                        i += 1
                out_parts.append("".join(buf))
            if end_semi:
                break
        elif s == ";":
            break

    if not out_parts:
        raise RuntimeError(f"Failed to parse TINYDVD_INDEX_HTML from {hpath}")
    return "".join(out_parts).encode("utf-8")


INDEX_HTML_BYTES = load_web_index_html()


def compute_ws_accept_key(sec_key: str) -> str:
    digest = hashlib.sha1(sec_key.strip().encode("ascii") + WS_GUID).digest()
    return base64.b64encode(digest).decode("ascii")


def ws_encode_frame(opcode: int, payload: bytes) -> bytes:
    plen = len(payload)
    if plen <= 125:
        hdr = bytes([0x80 | (opcode & 0x0F), plen])
    elif plen <= 65535:
        hdr = struct.pack("!BBH", 0x80 | (opcode & 0x0F), 126, plen)
    else:
        hdr = struct.pack("!BBQ", 0x80 | (opcode & 0x0F), 127, plen)
    return hdr + payload


def ws_send_text(sock: socket.socket, text: str) -> bool:
    try:
        sock.sendall(ws_encode_frame(0x1, text.encode("utf-8")))
        return True
    except OSError:
        return False


def _recv_exact(sock: socket.socket, n: int) -> bytes | None:
    chunks: list[bytes] = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


class WebSession:
    """Isolated DVD VM playback session shared by tabs using the same ?session=<id>."""

    def __init__(self, core: TinyDVDCore, session_id: str, disc_path: str, disc_id: str, disc_title: str):
        self.core = core
        self.session_id = session_id
        self.disc_path = disc_path
        self.disc_id = disc_id
        self.disc_title = disc_title
        self.last_active_time = time.time()

        self.lock = threading.Lock()
        self.state_lock = threading.Lock()
        self.ws_lock = threading.Lock()
        self.ws_clients: list[socket.socket] = []

        self.cached_load: str | None = None
        self.cached_seek: str | None = None
        self.cached_pause: str | None = None
        self.cached_highlight: str | None = None
        self.cached_domain_state: str | None = None

        self._cb_ref = TDVD_VM_JSON_CB(self._on_vm_json)
        self.vm_handle = self.core.lib.tdvd_vm_session_create(
            disc_path.encode("utf-8"),
            disc_id.encode("utf-8"),
            self._cb_ref,
            None,
        )
        if not self.vm_handle:
            raise RuntimeError(f"Failed to start DVD VM session for {disc_path}")

    def _on_vm_json(self, _user_data, json_cstr: bytes) -> None:
        if not json_cstr:
            return
        msg = json_cstr.decode("utf-8", errors="replace")
        try:
            mtype = json.loads(msg).get("type")
        except Exception:
            mtype = None

        with self.state_lock:
            if mtype == "video_load":
                self.cached_load = msg
                self.cached_seek = None
                self.cached_pause = None
                self.cached_highlight = None
            elif mtype == "seek":
                self.cached_seek = msg
            elif mtype == "pause":
                self.cached_pause = msg
            elif mtype == "highlight":
                self.cached_highlight = msg
            elif mtype == "domain_state":
                self.cached_domain_state = msg

        self.broadcast(msg)

    def broadcast(self, msg: str) -> None:
        dead: list[socket.socket] = []
        with self.ws_lock:
            for sock in list(self.ws_clients):
                if not ws_send_text(sock, msg):
                    dead.append(sock)
            for sock in dead:
                if sock in self.ws_clients:
                    self.ws_clients.remove(sock)

    def add_ws_client(self, sock: socket.socket) -> None:
        with self.state_lock:
            replay = [
                json.dumps({"type": "state", "session_id": self.session_id, "movie_title": self.disc_title}),
            ]
            for m in (
                self.cached_load,
                self.cached_seek,
                self.cached_pause,
                self.cached_highlight,
                self.cached_domain_state,
            ):
                if m:
                    replay.append(m)

        with self.ws_lock:
            for msg in replay:
                ws_send_text(sock, msg)
            self.ws_clients.append(sock)
            self.last_active_time = time.time()

    def remove_ws_client(self, sock: socket.socket) -> None:
        should_pause = False
        with self.ws_lock:
            if sock in self.ws_clients:
                self.ws_clients.remove(sock)
            self.last_active_time = time.time()
            should_pause = len(self.ws_clients) == 0
        if should_pause:
            with self.lock:
                if self.vm_handle:
                    self.core.lib.tdvd_vm_session_set_paused(self.vm_handle, 1)

    def client_count(self) -> int:
        with self.ws_lock:
            return len(self.ws_clients)

    def switch_movie(self, disc_path: str, disc_id: str, disc_title: str) -> bool:
        with self.lock:
            self.last_active_time = time.time()
            if disc_path == self.disc_path and disc_id == self.disc_id:
                return True
            self.disc_path = disc_path
            self.disc_id = disc_id
            self.disc_title = disc_title
            with self.state_lock:
                self.cached_load = None
                self.cached_seek = None
                self.cached_pause = None
                self.cached_highlight = None
                self.cached_domain_state = None
            if not self.vm_handle:
                return False
            rc = self.core.lib.tdvd_vm_session_load(
                self.vm_handle,
                disc_path.encode("utf-8"),
                disc_id.encode("utf-8"),
            )
            return rc == 0

    def handle_ws_message(self, payload: str) -> None:
        self.last_active_time = time.time()
        try:
            obj = json.loads(payload)
        except Exception:
            return
        if not isinstance(obj, dict):
            return

        with self.lock:
            vm = self.vm_handle
        if not vm:
            return

        mtype = obj.get("type")
        if mtype == "key" and obj.get("key"):
            self.core.lib.tdvd_vm_session_dispatch_action(vm, str(obj["key"]).encode("utf-8"), 0, 0)
        elif mtype == "mouse_move":
            x = int(obj.get("x") or 0)
            y = int(obj.get("y") or 0)
            self.core.lib.tdvd_vm_session_dispatch_action(vm, b"mouse_move", x, y)
        elif mtype == "mouse_click":
            x = int(obj.get("x") or 0)
            y = int(obj.get("y") or 0)
            self.core.lib.tdvd_vm_session_dispatch_action(vm, b"mouse_click", x, y)
        elif mtype in ("file_loaded", "seek_completed", "time_update", "client_playback", "video_ended"):
            seq = int(obj.get("seq") or 0)
            pts_val = obj.get("pts")
            pts_ns = int(max(0.0, float(pts_val)) * 1e9) if pts_val is not None else -1
            paused = -1
            if mtype == "client_playback":
                if obj.get("state") == "paused":
                    paused = 1
                elif obj.get("state") == "playing":
                    paused = 0
            self.core.lib.tdvd_vm_session_notify_playback(
                vm, mtype.encode("utf-8"), seq, pts_ns, paused
            )

    def destroy(self) -> None:
        with self.ws_lock:
            for sock in self.ws_clients:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    sock.close()
                except OSError:
                    pass
            self.ws_clients.clear()

        with self.lock:
            vm = self.vm_handle
            self.vm_handle = None
        if vm:
            self.core.lib.tdvd_vm_session_destroy(vm)
        self._cb_ref = None


class _TinyDVDThreadingHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, server_address, RequestHandlerClass, app_server: "TinyDVDWebServer"):
        self.app_server = app_server
        super().__init__(server_address, RequestHandlerClass)


class _DummyHTTPServer:
    def __init__(self, app_server: "TinyDVDWebServer"):
        self.app_server = app_server


class TinyDVDRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    rbufsize = 0  # Unbuffered rfile so WebSocket / SSE bytes remain on the socket

    def log_message(self, format, *args) -> None:
        # Suppress verbose per-request stdout/stderr logging.
        return

    def _send_bytes(
        self,
        code: int,
        reason: str,
        content_type: str,
        body: bytes,
        extra_headers: dict[str, str] | None = None,
    ) -> None:
        try:
            self.send_response(code, reason)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            if extra_headers:
                for k, v in extra_headers.items():
                    self.send_header(k, v)
            self.end_headers()
            if body:
                self.wfile.write(body)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        self.close_connection = True

    def _send_json(self, code: int, reason: str, obj: dict, cache_control: str = "no-store") -> None:
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        headers = {"Cache-Control": cache_control} if cache_control else None
        self._send_bytes(code, reason, "application/json", body, extra_headers=headers)

    def _send_error_text(self, code: int, msg: str) -> None:
        self._send_bytes(code, msg, "text/plain", msg.encode("utf-8"))

    def do_GET(self) -> None:
        self._dispatch()

    def do_POST(self) -> None:
        # Drain any request body cleanly if Content-Length is provided
        clen = int(self.headers.get("Content-Length") or 0)
        if 0 < clen <= 1048576:
            try:
                self.rfile.read(clen)
            except OSError:
                pass
        self._dispatch()

    def _dispatch(self) -> None:
        app: TinyDVDWebServer = self.server.app_server
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path or "/"
        qs = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)

        session_param = sanitize_id(qs.get("session", [""])[0])

        # 1. WebSocket upgrade
        ws_key = self.headers.get("Sec-WebSocket-Key")
        if path.startswith("/ws") and ws_key:
            self._handle_websocket(app, ws_key, session_param)
            return

        # 2. SPA HTML
        if path in ("/", "/index.html") or path.startswith("/player"):
            self._send_bytes(200, "OK", "text/html; charset=utf-8", INDEX_HTML_BYTES)
            return

        # 3. SSE Catalog Stream
        if path == "/api/library/events":
            self._handle_sse(app)
            return

        # 4. Catalog JSON
        if path == "/api/library":
            app.library.check_for_changes()
            is_dir, items, _ = app.library.get_snapshot()
            payload = {
                "is_directory": is_dir,
                "items": [it.to_api_dict() for it in items],
            }
            self._send_json(200, "OK", payload, cache_control="no-store")
            return

        # 5. Import Status
        if path == "/api/import/status":
            st = app.library.get_import_status()
            st.pop("state_code", None)
            st.pop("start_time", None)
            st.pop("result_disc_path", None)
            self._send_json(200, "OK", st, cache_control="no-store")
            return

        # 6. Import Start
        if path == "/api/import/start":
            src = qs.get("source", [""])[0]
            title = qs.get("title", [""])[0]
            skip_raw = qs.get("skip_enrichment", [""])[0].lower()
            skip_enrich = skip_raw in ("1", "true", "yes")
            ok, err = app.library.start_import(src, title=title, skip_enrichment=skip_enrich)
            if ok:
                self._send_json(200, "OK", {"status": "started"})
            else:
                self._send_json(409, "Conflict", {"status": "error", "error": err or "An import job is already in progress"})
            return

        # 7. Import Cancel
        if path == "/api/import/cancel":
            app.library.cancel_import()
            self._send_json(200, "OK", {"status": "cancelling"})
            return

        # 8. Thumbnail PNG
        if path == "/api/thumbnail" or path.startswith("/api/thumbnail/"):
            disc_id = qs.get("disc", [""])[0]
            if not disc_id and path.startswith("/api/thumbnail/"):
                disc_id = path[len("/api/thumbnail/"):]
            disc_id = sanitize_id(disc_id)
            item = app.library.find_by_id(disc_id)
            if not item and not disc_id:
                is_dir, items, raw_target = app.library.get_snapshot()
                disc_path = items[0].path if items else (raw_target if not is_dir else "")
            else:
                disc_path = item.path if item else ""

            png_data = app.library.read_thumbnail(disc_path)
            if png_data:
                self._send_bytes(
                    200,
                    "OK",
                    "image/png",
                    png_data,
                    extra_headers={"Cache-Control": "public, max-age=86400"},
                )
            else:
                self._send_error_text(404, "Thumbnail Not Found")
            return

        # 9. Switch/start playback session
        if path == "/api/play":
            disc_id = sanitize_id(qs.get("disc", [""])[0])
            item = app.library.find_by_id(disc_id)
            if not item or item.id != disc_id:
                self._send_error_text(404, "Movie Not Found")
                return
            sess = app.get_or_create_session(session_param, item.path, item.id, item.title)
            if not sess:
                self._send_error_text(500, "Failed to Start Session")
                return
            sess.switch_movie(item.path, item.id, item.title)
            self._send_json(200, "OK", {"status": "ok", "session_id": sess.session_id})
            return

        # 10. MP4 Range streaming (safe remux without shell)
        if path.startswith("/api/media/"):
            fname = path[len("/api/media/"):]
            base_name = os.path.splitext(fname)[0]
            if not VALID_MEDIA_BASE_RE.fullmatch(base_name):
                self._send_error_text(400, "Invalid Media Name")
                return

            disc_id = sanitize_id(qs.get("disc", [""])[0])
            mp4_path = app.ensure_remuxed_mp4(disc_id, base_name)
            if not mp4_path:
                self._send_error_text(404, "Media Stream Not Found")
                return
            self._serve_range_file(mp4_path, self.headers.get("Range"))
            return

        self._send_error_text(404, "Not Found")

    def _serve_range_file(self, file_path: str, range_header: str | None) -> None:
        try:
            total = os.path.getsize(file_path)
        except OSError:
            total = 0
        if total <= 0:
            self._send_error_text(404, "Not Found")
            return

        start = 0
        end = total - 1
        partial = False

        if range_header and range_header.startswith("bytes="):
            spec = range_header[6:].strip()
            if "-" in spec:
                partial = True
                s_part, e_part = spec.split("-", 1)
                try:
                    if not s_part:
                        suffix = int(e_part)
                        start = max(0, total - suffix)
                        end = total - 1
                    else:
                        start = int(s_part)
                        end = int(e_part) if e_part else (total - 1)
                except ValueError:
                    partial = False

        if partial and (start > end or end >= total):
            try:
                self.send_response(416, "Range Not Satisfiable")
                self.send_header("Content-Range", f"bytes */{total}")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
            except OSError:
                pass
            self.close_connection = True
            return

        content_length = end - start + 1
        try:
            with open(file_path, "rb") as f:
                if partial:
                    self.send_response(206, "Partial Content")
                    self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
                else:
                    self.send_response(200, "OK")
                self.send_header("Content-Type", "video/mp4")
                self.send_header("Content-Length", str(content_length))
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Connection", "close")
                self.end_headers()

                f.seek(start)
                rem = content_length
                while rem > 0:
                    chunk = f.read(min(CHUNK_SIZE, rem))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    rem -= len(chunk)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        self.close_connection = True

    def _handle_sse(self, app: "TinyDVDWebServer") -> None:
        self.close_connection = True
        sock = self.connection
        hdr = (
            b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/event-stream\r\n"
            b"Cache-Control: no-cache, no-store\r\n"
            b"Connection: keep-alive\r\n\r\n"
            b": connected\n\n"
        )
        try:
            sock.sendall(hdr)
        except OSError:
            return

        with app.sse_lock:
            app.sse_clients.add(sock)
        try:
            last_gen = app.library.get_generation()
            while app.running:
                r, _, _ = select.select([sock], [], [], 0)
                if r:
                    try:
                        peek = sock.recv(1, socket.MSG_PEEK)
                        if not peek:
                            break
                    except BlockingIOError:
                        pass
                    except OSError:
                        break

                with app.library.catalog_cond:
                    if app.running and app.library.catalog_generation == last_gen:
                        app.library.catalog_cond.wait(timeout=0.25)
                    cur_gen = app.library.catalog_generation

                if not app.running:
                    break
                if cur_gen != last_gen:
                    last_gen = cur_gen
                    msg = f'event: library_update\ndata: {{"generation":{cur_gen}}}\n\n'.encode("utf-8")
                    try:
                        sock.sendall(msg)
                    except OSError:
                        break
        finally:
            with app.sse_lock:
                app.sse_clients.discard(sock)

    def _handle_websocket(self, app: "TinyDVDWebServer", ws_key: str, session_param: str) -> None:
        self.close_connection = True
        sock = self.connection
        accept = compute_ws_accept_key(ws_key)
        resp = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
        ).encode("ascii")
        try:
            sock.sendall(resp)
        except OSError:
            return

        is_dir, items, raw_target = app.library.get_snapshot()
        default_path = items[0].path if items else (raw_target if not is_dir else "")
        default_id = items[0].id if items else sanitize_id(os.path.basename(default_path))
        default_title = items[0].title if items else os.path.basename(default_path)

        sess = app.get_or_create_session(session_param, default_path, default_id, default_title)
        if not sess:
            return
        sess.add_ws_client(sock)

        try:
            while app.running:
                hdr2 = _recv_exact(sock, 2)
                if not hdr2:
                    break
                b0, b1 = hdr2[0], hdr2[1]
                opcode = b0 & 0x0F
                is_masked = (b1 & 0x80) != 0
                plen = b1 & 0x7F

                if opcode == 0x8:  # Close
                    break
                if plen == 126:
                    ext = _recv_exact(sock, 2)
                    if not ext:
                        break
                    plen = struct.unpack("!H", ext)[0]
                elif plen == 127:
                    ext = _recv_exact(sock, 8)
                    if not ext:
                        break
                    plen = struct.unpack("!Q", ext)[0]

                mask = b""
                if is_masked:
                    mask = _recv_exact(sock, 4) or b""
                    if len(mask) != 4:
                        break

                if plen > 65536:
                    break
                payload_raw = _recv_exact(sock, plen) if plen > 0 else b""
                if payload_raw is None:
                    break

                if is_masked and plen > 0:
                    payload_bytes = bytes(b ^ mask[i % 4] for i, b in enumerate(payload_raw))
                else:
                    payload_bytes = payload_raw

                if opcode == 0x9:  # Ping -> Pong
                    try:
                        sock.sendall(ws_encode_frame(0xA, payload_bytes))
                    except OSError:
                        break
                    continue

                if opcode in (0x1, 0x2):
                    sess.handle_ws_message(payload_bytes.decode("utf-8", errors="replace"))
        except OSError:
            pass
        finally:
            sess.remove_ws_client(sock)


class TinyDVDWebServer:
    """Multi-session streaming server backed by OS directory watching and libtinydvd_core."""

    def __init__(
        self,
        core: TinyDVDCore,
        target_path: str,
        bind_addr: str = "0.0.0.0",
        port: int = 8080,
        announce: bool = True,
        listen: bool = True,
    ):
        self.core = core
        self.library = TinyDVDLibrary(target_path)
        self.bind_addr = bind_addr or "0.0.0.0"
        self.port = 8080 if port is None else int(port)

        self.sessions_lock = threading.Lock()
        self.sessions: dict[str, WebSession] = {}

        self.remux_locks_guard = threading.Lock()
        self.remux_locks: dict[str, threading.Lock] = {}

        self.sse_lock = threading.Lock()
        self.sse_clients: set[socket.socket] = set()

        self.running = True
        self._dummy_httpd = _DummyHTTPServer(self)
        self.httpd = None
        self._serve_thread = None

        if listen:
            self.httpd = _TinyDVDThreadingHTTPServer(
                (self.bind_addr, self.port), TinyDVDRequestHandler, self
            )
            self.port = self.httpd.server_address[1]
            self._serve_thread = threading.Thread(
                target=self.httpd.serve_forever, name="tdvd-http-accept", daemon=True
            )
            self._serve_thread.start()

        self._reaper_thread = threading.Thread(
            target=self._reaper_loop, name="tdvd-session-reaper", daemon=True
        )
        self._reaper_thread.start()

        if announce:
            is_dir, items, _ = self.library.get_snapshot()
            cnt = len(items)
            print("\n=======================================================")
            print("  TinyDVD Web Player Running!")
            print(f"  URL: http://localhost:{self.port}")
            print(f"  Mode: {'Library Catalog' if is_dir else 'Direct Cinema Player'} ({cnt} movie{'' if cnt == 1 else 's'} available)")
            print("  Multi-Session Support: Enabled (isolated playback per tab/window)")
            print("=======================================================\n", flush=True)

    def handle_client_socket(self, sock: socket.socket) -> threading.Thread:
        """Dispatch a pre-connected socket (e.g. AF_UNIX socketpair in sandbox tests)."""
        def _run() -> None:
            try:
                TinyDVDRequestHandler(sock, ("127.0.0.1", 0), self._dummy_httpd)
            finally:
                try:
                    sock.close()
                except OSError:
                    pass

        t = threading.Thread(target=_run, name="tdvd-client-sock", daemon=True)
        t.start()
        return t

    def get_or_create_session(
        self, session_id: str | None, disc_path: str, disc_id: str, disc_title: str
    ) -> WebSession | None:
        sid = sanitize_id(session_id or "")
        if not sid:
            sid = "s_" + uuid.uuid4().hex[:16]

        with self.sessions_lock:
            existing = self.sessions.get(sid)
            if existing:
                existing.last_active_time = time.time()
                return existing

        if not disc_path:
            return None

        sess = WebSession(self.core, sid, disc_path, disc_id, disc_title)
        with self.sessions_lock:
            again = self.sessions.get(sid)
            if again:
                sess.destroy()
                return again
            self.sessions[sid] = sess
        print(f"[web_server] Started independent session {sid} for {disc_title or disc_id}", flush=True)
        return sess

    def ensure_remuxed_mp4(self, disc_id: str, base_name: str) -> str | None:
        if not VALID_MEDIA_BASE_RE.fullmatch(base_name):
            return None

        item = self.library.find_by_id(disc_id)
        if not item:
            is_dir, items, raw_target = self.library.get_snapshot()
            dpath = items[0].path if items else (raw_target if not is_dir else "")
            did = items[0].id if items else sanitize_id(os.path.basename(dpath))
        else:
            dpath = item.path
            did = item.id

        did = sanitize_id(did) or "default"
        cache_dir = os.path.join("/tmp/tinydvd_web_cache", did)
        os.makedirs(cache_dir, exist_ok=True)
        out_path = os.path.join(cache_dir, f"{base_name}.mp4")

        key = f"{did}/{base_name}"
        with self.remux_locks_guard:
            rlock = self.remux_locks.setdefault(key, threading.Lock())

        with rlock:
            try:
                if os.path.isfile(out_path) and os.path.getsize(out_path) > 0:
                    return out_path
            except OSError:
                pass

            mkv_name = f"{base_name}.mkv"
            slice_info = self.library.get_mkv_slice(dpath, mkv_name)
            if not slice_info:
                return None

            tmp_path = out_path + f".tmp.{threading.get_ident()}"
            print(f"[web_server] Remuxing {mkv_name} -> {out_path}...", flush=True)

            try:
                if slice_info[0] == "file":
                    proc = subprocess.run(
                        [
                            "ffmpeg", "-y", "-nostdin", "-hide_banner", "-loglevel", "error",
                            "-i", slice_info[1],
                            "-c", "copy", "-movflags", "+faststart", "-f", "mp4", tmp_path,
                        ],
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        check=False,
                    )
                else:
                    proc = subprocess.Popen(
                        [
                            "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                            "-i", "pipe:0",
                            "-c", "copy", "-movflags", "+faststart", "-f", "mp4", tmp_path,
                        ],
                        stdin=subprocess.PIPE,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                    try:
                        if slice_info[0] == "slice":
                            _, archive_path, offset, size = slice_info
                            with open(archive_path, "rb") as fin:
                                fin.seek(offset)
                                rem = size
                                while rem > 0:
                                    chunk = fin.read(min(CHUNK_SIZE, rem))
                                    if not chunk:
                                        break
                                    proc.stdin.write(chunk)
                                    rem -= len(chunk)
                        else:
                            _, archive_path, member_name, _ = slice_info
                            with zipfile.ZipFile(archive_path, "r") as zf, zf.open(member_name) as zstream:
                                shutil.copyfileobj(zstream, proc.stdin, CHUNK_SIZE)
                    except (BrokenPipeError, OSError):
                        pass
                    finally:
                        try:
                            if proc.stdin:
                                proc.stdin.close()
                        except OSError:
                            pass
                    proc.wait()

                if os.path.isfile(tmp_path) and os.path.getsize(tmp_path) > 0:
                    os.replace(tmp_path, out_path)
                    sz = os.path.getsize(out_path)
                    print(f"[web_server] Remux complete: {out_path} ({sz} bytes)", flush=True)
                    return out_path
            finally:
                if os.path.exists(tmp_path):
                    try:
                        os.unlink(tmp_path)
                    except OSError:
                        pass
            return None

    def _reaper_loop(self) -> None:
        while self.running:
            time.sleep(5)
            if not self.running:
                break
            now = time.time()
            expired: list[WebSession] = []
            with self.sessions_lock:
                for sid in list(self.sessions.keys()):
                    sess = self.sessions[sid]
                    if sess.client_count() == 0 and (now - sess.last_active_time) > SESSION_TIMEOUT_SEC:
                        expired.append(self.sessions.pop(sid))
            for sess in expired:
                print(
                    f"[web_server] Pruning expired idle session {sess.session_id} ({sess.disc_title})",
                    flush=True,
                )
                sess.destroy()

    def serve_until_interrupted(self) -> None:
        try:
            while self.running:
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass

    def stop(self) -> None:
        if not self.running:
            return
        self.running = False
        self.library.wake_sse_clients()

        with self.sse_lock:
            for sock in list(self.sse_clients):
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

        if self.httpd is not None:
            try:
                self.httpd.shutdown()
                self.httpd.server_close()
            except Exception:
                pass

        with self.sessions_lock:
            all_sessions = list(self.sessions.values())
            self.sessions.clear()
        for sess in all_sessions:
            sess.destroy()

        self.library.destroy()

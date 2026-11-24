#!/usr/bin/env python3
"""
TinyDVD web player: a small HTTP server (with Range requests, Server-Sent
Events and RFC 6455 WebSockets) backed by TinyDVDLibrary and the DVD VM
sessions in libtinydvd_core.

Security model:
  - Every page and API call needs a login (one shared password, see WebAuth).
  - Anything that changes state is a POST, and is refused if the browser says
    it came from another site (Origin check). The login cookie is
    SameSite=Strict as well.
  - Importing discs from the web is off unless the server is started with
    allow_import=True, because it runs ripping and conversion tools on
    whatever path the client names.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
from http.cookies import CookieError, SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import secrets
import select
import socket
import struct
import threading
import time
import urllib.parse
import uuid

from tinydvd_core import TDVD_VM_JSON_CB, TinyDVDCore
import tinydvd_archive
import tinydvd_mp4
from tinydvd_library import TinyDVDLibrary, disc_id_for_path, sanitize_id


WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
CHUNK_SIZE = 65536
SESSION_TIMEOUT_SEC = 180
MAX_SESSIONS = 32
MAX_REQUEST_BODY = 64 * 1024
WS_SEND_TIMEOUT_SEC = 2
# A disc's video and audio files: NAME_VIDEO.mp4, NAME_AUDIO_1.mp4, ...
MEDIA_NAME_RE = re.compile(r"^[A-Za-z0-9_]+_(?:VIDEO|AUDIO_[0-9]+)\.mp4$")

AUTH_COOKIE = "tinydvd_auth"
AUTH_LIFETIME_SEC = 30 * 24 * 3600
LOGIN_MAX_FAILURES = 5
LOGIN_LOCKOUT_SEC = 60

HTML_ROUTES = ("/", "/index.html")

CLIENT_LOG_LIMIT = 50

# Every video streams as HLS, in segments of about this long: see
# TinyDVDWebServer.playlist
HLS_SEGMENT_SECONDS = 4.0


def load_web_index_html() -> bytes:
    """The web player page, web/index.html (next to scripts/ in the source tree and when installed)."""
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web", "index.html")
    with open(path, "rb") as f:
        return f.read()


INDEX_HTML_BYTES = load_web_index_html()


def load_vendor_file(name: str) -> bytes:
    """A third-party file the web player uses, from web/vendor/ (b"" if missing)."""
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web", "vendor", name)
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return b""


HLS_JS_BYTES = load_vendor_file("hls.min.js")


def load_web_file(name: str) -> bytes:
    """A file from web/ (b"" if missing)."""
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web", name)
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return b""


# For "Add to Home Screen". Phones fetch these without the login cookie, so
# they're served without a login; they hold nothing private
PUBLIC_FILES = {
    "/manifest.webmanifest": ("application/manifest+json", load_web_file("manifest.webmanifest")),
    "/apple-touch-icon.png": ("image/png", load_web_file("apple-touch-icon.png")),
    "/icon-192.png": ("image/png", load_web_file("icon-192.png")),
    "/icon-512.png": ("image/png", load_web_file("icon-512.png")),
}

LOGIN_PAGE_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>TinyDVD Login</title>
<style>
  body { margin: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center;
         background: #0b0d12; color: #e8e8ee; font: 16px/1.4 system-ui, sans-serif; }
  form { background: #161a22; padding: 28px; border-radius: 12px; width: min(320px, calc(100vw - 32px));
         box-sizing: border-box; }
  h1 { margin: 0 0 16px; font-size: 1.3rem; }
  h1 span { color: #f5a623; }
  input { width: 100%; box-sizing: border-box; padding: 10px; margin: 0 0 12px; border-radius: 8px;
          border: 1px solid #333a48; background: #0b0d12; color: inherit; font-size: 1rem; }
  button { width: 100%; padding: 10px; border: 0; border-radius: 8px; background: #f5a623;
           color: #111; font-weight: 600; font-size: 1rem; cursor: pointer; }
  .error { color: #ff6b6b; margin: 0 0 12px; font-size: 0.9rem; }
  .hint { color: #8a90a0; font-size: 0.8rem; margin: 12px 0 0; }
</style>
</head>
<body>
<form method="post" action="/login">
  <h1>Tiny<span>DVD</span></h1>
  {error}
  <input type="password" name="password" placeholder="Password" autocomplete="current-password" autofocus required>
  <button type="submit">Log in</button>
  <p class="hint">The password was printed when the server started.</p>
</form>
</body>
</html>
"""


class WebAuth:
    """
    One shared password. A successful login gets a signed, expiring cookie,
    so no session table is needed and logins survive a server restart.
    Changing the password signs everyone out.
    """

    def __init__(self, password: str):
        if not password:
            raise ValueError("A web password is required")
        self._password_digest = hashlib.sha256(password.encode("utf-8")).digest()
        self._key = hashlib.sha256(b"tinydvd-web-auth\0" + password.encode("utf-8")).digest()
        self._revoked: set[str] = set()
        self._lock = threading.Lock()
        self._failures: dict[str, tuple[int, float]] = {}

    def check_password(self, attempt: str) -> bool:
        digest = hashlib.sha256((attempt or "").encode("utf-8")).digest()
        return hmac.compare_digest(digest, self._password_digest)

    def _sign(self, payload: str) -> str:
        return hmac.new(self._key, payload.encode("ascii"), hashlib.sha256).hexdigest()

    def issue_token(self) -> str:
        payload = f"{int(time.time()) + AUTH_LIFETIME_SEC}.{secrets.token_hex(8)}"
        return f"{payload}.{self._sign(payload)}"

    def token_valid(self, token: str) -> bool:
        parts = (token or "").split(".")
        if len(parts) != 3 or not parts[0].isdigit():
            return False
        payload = f"{parts[0]}.{parts[1]}"
        if not hmac.compare_digest(self._sign(payload), parts[2]):
            return False
        if int(parts[0]) < time.time():
            return False
        with self._lock:
            return token not in self._revoked

    def revoke(self, token: str) -> None:
        if self.token_valid(token):
            with self._lock:
                self._revoked.add(token)

    def locked_out(self, client: str) -> bool:
        with self._lock:
            count, since = self._failures.get(client, (0, 0.0))
            if count >= LOGIN_MAX_FAILURES and time.time() - since < LOGIN_LOCKOUT_SEC:
                return True
            if time.time() - since >= LOGIN_LOCKOUT_SEC:
                self._failures.pop(client, None)
            return False

    def record_failure(self, client: str) -> None:
        with self._lock:
            count, since = self._failures.get(client, (0, time.time()))
            self._failures[client] = (count + 1, since)

    def record_success(self, client: str) -> None:
        with self._lock:
            self._failures.pop(client, None)


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


def _set_send_timeout(sock: socket.socket, seconds: int) -> None:
    """Bound how long a send can block, without affecting the blocking reads on the same socket."""
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO, struct.pack("ll", seconds, 0))
    except (OSError, struct.error):
        pass


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


def _as_int(val, default: int = 0) -> int:
    try:
        return int(val)
    except (TypeError, ValueError):
        return default


class WebSession:
    """
    Isolated DVD VM playback session shared by tabs using the same ?session=<id>.
    A session can start with no disc (disc_path ""): its VM is created when a
    movie is picked, so the library page isn't replaced by a movie nobody chose.
    """

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
        self.cached_audio_track: str | None = None
        self.client_log_count = 0

        self._cb_ref = TDVD_VM_JSON_CB(self._on_vm_json)
        self.vm_handle = None
        if disc_path:
            self.vm_handle = self._create_vm(disc_path, disc_id)
            if not self.vm_handle:
                raise RuntimeError(f"Failed to start DVD VM session for {disc_path}")

    def _create_vm(self, disc_path: str, disc_id: str):
        return self.core.lib.tdvd_vm_session_create(
            disc_path.encode("utf-8"),
            disc_id.encode("utf-8"),
            self._cb_ref,
            None,
        )

    def _on_vm_json(self, _user_data, json_cstr: bytes) -> None:
        if not json_cstr:
            return
        msg = json_cstr.decode("utf-8", errors="replace")
        try:
            mtype = json.loads(msg).get("type")
        except (ValueError, AttributeError):
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
            elif mtype == "audio_track":
                self.cached_audio_track = msg

        self.broadcast(msg)

    def broadcast(self, msg: str) -> None:
        # Runs on the VM thread. Client sockets have a send timeout, so one
        # stalled browser can hold this up for seconds at most, then is dropped.
        dead: list[socket.socket] = []
        with self.ws_lock:
            for sock in list(self.ws_clients):
                if not ws_send_text(sock, msg):
                    dead.append(sock)
            for sock in dead:
                if sock in self.ws_clients:
                    self.ws_clients.remove(sock)
        for sock in dead:
            try:
                sock.shutdown(socket.SHUT_RDWR)  # ends that client's reader thread
            except OSError:
                pass

    def add_ws_client(self, sock: socket.socket) -> None:
        _set_send_timeout(sock, WS_SEND_TIMEOUT_SEC)
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
                self.cached_audio_track,
            ):
                if m:
                    replay.append(m)

        with self.ws_lock:
            for msg in replay:
                ws_send_text(sock, msg)
            self.ws_clients.append(sock)
            self.last_active_time = time.time()

    def remove_ws_client(self, sock: socket.socket) -> None:
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
                self.cached_audio_track = None
            if not self.vm_handle:
                # First movie picked in this session
                self.vm_handle = self._create_vm(disc_path, disc_id)
                return bool(self.vm_handle)
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
        except ValueError:
            return
        if not isinstance(obj, dict):
            return

        mtype = obj.get("type")
        if mtype == "client_log" and isinstance(obj.get("text"), str):
            # A problem the browser reports: printable characters only, and at
            # most CLIENT_LOG_LIMIT lines per session
            self.client_log_count += 1
            if self.client_log_count <= CLIENT_LOG_LIMIT:
                text = "".join(ch for ch in obj["text"][:200] if ch.isprintable())
                print(f"[web_server] Browser ({self.session_id}): {text}", flush=True)
            return

        with self.lock:
            vm = self.vm_handle
        if not vm:
            return

        if mtype == "key" and isinstance(obj.get("key"), str):
            self.core.lib.tdvd_vm_session_dispatch_action(vm, obj["key"].encode("utf-8"), 0, 0, 0.0)
        elif mtype in ("mouse_move", "mouse_click"):
            x = _as_int(obj.get("x"))
            y = _as_int(obj.get("y"))
            self.core.lib.tdvd_vm_session_dispatch_action(vm, mtype.encode("ascii"), x, y, 0.0)
        elif mtype == "seek_to":
            # The timeline: the DVD engine does the seek (within the title
            # playing), so it knows where playback is
            try:
                pts = float(obj.get("pts"))
            except (TypeError, ValueError):
                return
            if pts >= 0.0 and pts < 1e7:
                self.core.lib.tdvd_vm_session_dispatch_action(vm, b"seek_abs", 0, 0, pts)
        elif mtype in ("file_loaded", "seek_completed", "time_update", "client_playback", "video_ended",
                       "loading", "buffering", "video_error"):
            seq = max(0, _as_int(obj.get("seq")))
            pts_ns = -1
            try:
                if obj.get("pts") is not None:
                    pts_ns = int(max(0.0, float(obj["pts"])) * 1e9)
            except (TypeError, ValueError, OverflowError):
                pts_ns = -1
            paused = -1
            if mtype == "buffering":
                paused = 1 if obj.get("buffering") else 0  # carries the buffering flag
            elif mtype == "client_playback":
                if obj.get("state") == "paused":
                    paused = 1
                elif obj.get("state") == "playing":
                    paused = 0
            self.core.lib.tdvd_vm_session_notify_playback(
                vm, mtype.encode("ascii"), seq, pts_ns, paused
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

    # -- Responses ---------------------------------------------------------

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
            self.send_header("X-Content-Type-Options", "nosniff")
            if extra_headers:
                for k, v in extra_headers.items():
                    self.send_header(k, v)
            self.end_headers()
            if body:
                self.wfile.write(body)
                self.wfile.flush()
        except OSError:
            pass
        self.close_connection = True

    def _send_json(self, code: int, reason: str, obj: dict, cache_control: str = "no-store") -> None:
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        headers = {"Cache-Control": cache_control} if cache_control else None
        self._send_bytes(code, reason, "application/json", body, extra_headers=headers)

    def _send_error_text(self, code: int, msg: str) -> None:
        self._send_bytes(code, msg, "text/plain", msg.encode("utf-8"))

    def _redirect(self, location: str, extra_headers: dict[str, str] | None = None) -> None:
        headers = {"Location": location, "Cache-Control": "no-store"}
        headers.update(extra_headers or {})
        self._send_bytes(303, "See Other", "text/plain", b"", extra_headers=headers)

    # -- Request plumbing --------------------------------------------------

    def do_GET(self) -> None:
        self._dispatch("GET", b"")

    def do_POST(self) -> None:
        try:
            clen = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            clen = -1
        if clen < 0 or clen > MAX_REQUEST_BODY:
            self._send_error_text(413, "Request Too Large")
            return
        try:
            body = self.rfile.read(clen) if clen else b""
        except OSError:
            return
        self._dispatch("POST", body)

    def _auth_token(self) -> str:
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get("Cookie") or "")
        except CookieError:
            return ""
        morsel = cookie.get(AUTH_COOKIE)
        return morsel.value if morsel else ""

    def _same_origin(self) -> bool:
        """
        False if a browser says this request came from another site. Browsers
        send Origin on every cross-site POST and WebSocket handshake; clients
        that send none (curl, tests) aren't browsers, so there is no
        cross-site request to forge.
        """
        origin = self.headers.get("Origin")
        if not origin:
            return True
        return urllib.parse.urlsplit(origin).netloc.lower() == (self.headers.get("Host") or "").lower()

    def _client_key(self) -> str:
        addr = self.client_address
        return str(addr[0]) if isinstance(addr, tuple) and addr else "local"

    @staticmethod
    def _json_body(body: bytes) -> dict:
        try:
            obj = json.loads(body.decode("utf-8")) if body else {}
        except (UnicodeDecodeError, ValueError):
            return {}
        return obj if isinstance(obj, dict) else {}

    # -- Routing -----------------------------------------------------------

    def _dispatch(self, method: str, body: bytes) -> None:
        app: TinyDVDWebServer = self.server.app_server
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path or "/"
        qs = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)

        if method == "GET" and path in PUBLIC_FILES and PUBLIC_FILES[path][1]:
            content_type, data = PUBLIC_FILES[path]
            self._send_bytes(200, "OK", content_type, data, extra_headers={"Cache-Control": "public, max-age=86400"})
            return

        if path == "/login":
            if method == "POST":
                self._handle_login(app, body)
            else:
                self._send_login_page()
            return

        if not app.auth.token_valid(self._auth_token()):
            if method == "GET" and (path in HTML_ROUTES or path.startswith("/player")):
                self._redirect("/login")
            else:
                self._send_json(401, "Unauthorized", {"error": "Log in first"})
            return

        if method == "POST" and not self._same_origin():
            self._send_json(403, "Forbidden", {"error": "Cross-site request refused"})
            return

        ws_key = self.headers.get("Sec-WebSocket-Key")
        if method == "GET" and path.startswith("/ws") and ws_key:
            if not self._same_origin():
                self._send_error_text(403, "Forbidden")
                return
            self._handle_websocket(app, ws_key, sanitize_id(qs.get("session", [""])[0]))
            return

        if method == "GET":
            handler = {
                "/api/library": self._get_library,
                "/api/library/events": self._handle_sse,
                "/api/import/status": self._get_import_status,
            }.get(path)
            if path in HTML_ROUTES or path.startswith("/player"):
                self._send_bytes(200, "OK", "text/html; charset=utf-8", INDEX_HTML_BYTES,
                                 extra_headers={"Cache-Control": "no-store"})
            elif handler:
                handler(app)
            elif path == "/api/thumbnail" or path.startswith("/api/thumbnail/"):
                self._get_thumbnail(app, path, qs)
            elif path.startswith("/api/media/"):
                self._get_media(app, path, qs)
            elif path == "/vendor/hls.min.js" and HLS_JS_BYTES:
                # The page asks for it with ?v=<version>, so it can be cached for good
                self._send_bytes(200, "OK", "text/javascript; charset=utf-8", HLS_JS_BYTES,
                                 extra_headers={"Cache-Control": "private, max-age=31536000, immutable"})
            else:
                self._send_error_text(404, "Not Found")
            return

        if method == "POST":
            handler = {
                "/logout": self._post_logout,
                "/api/play": self._post_play,
                "/api/import/start": self._post_import_start,
                "/api/import/cancel": self._post_import_cancel,
            }.get(path)
            if handler:
                handler(app, self._json_body(body))
            else:
                self._send_error_text(404, "Not Found")
            return

        self._send_error_text(405, "Method Not Allowed")

    # -- Login -------------------------------------------------------------

    def _send_login_page(self, error: str = "", code: int = 200) -> None:
        err_html = f'<p class="error">{error}</p>' if error else ""
        page = LOGIN_PAGE_TEMPLATE.replace("{error}", err_html).encode("utf-8")
        self._send_bytes(code, "OK" if code == 200 else "Unauthorized", "text/html; charset=utf-8", page,
                         extra_headers={"Cache-Control": "no-store", "X-Frame-Options": "DENY"})

    def _handle_login(self, app: "TinyDVDWebServer", body: bytes) -> None:
        if not self._same_origin():
            self._send_error_text(403, "Forbidden")
            return
        client = self._client_key()
        if app.auth.locked_out(client):
            self._send_login_page("Too many attempts. Wait a minute and try again.", 429)
            return
        if (self.headers.get("Content-Type") or "").startswith("application/json"):
            attempt = str(self._json_body(body).get("password") or "")
        else:
            form = urllib.parse.parse_qs(body.decode("utf-8", errors="replace"))
            attempt = form.get("password", [""])[0]
        if not app.auth.check_password(attempt):
            app.auth.record_failure(client)
            time.sleep(0.5)
            self._send_login_page("Wrong password.", 401)
            return
        app.auth.record_success(client)
        cookie = (f"{AUTH_COOKIE}={app.auth.issue_token()}; Path=/; HttpOnly; SameSite=Strict; "
                  f"Max-Age={AUTH_LIFETIME_SEC}")
        self._redirect("/", {"Set-Cookie": cookie})

    def _post_logout(self, app: "TinyDVDWebServer", _body: dict) -> None:
        app.auth.revoke(self._auth_token())
        self._send_bytes(200, "OK", "application/json", b'{"status":"logged_out"}', extra_headers={
            "Cache-Control": "no-store",
            "Set-Cookie": f"{AUTH_COOKIE}=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0",
        })

    # -- API ---------------------------------------------------------------

    def _get_library(self, app: "TinyDVDWebServer") -> None:
        app.library.check_for_changes()
        is_dir, items, _ = app.library.get_snapshot()
        self._send_json(200, "OK", {
            "is_directory": is_dir,
            "import_enabled": app.allow_import,
            "items": [it.to_api_dict() for it in items],
        })

    def _get_import_status(self, app: "TinyDVDWebServer") -> None:
        if not app.allow_import:
            self._send_json(200, "OK", {"enabled": False, "state": "idle", "active": False})
            return
        st = app.library.get_import_status()
        for private in ("state_code", "start_time", "result_disc_path"):
            st.pop(private, None)
        st["enabled"] = True
        self._send_json(200, "OK", st)

    def _post_import_start(self, app: "TinyDVDWebServer", body: dict) -> None:
        if not app.allow_import:
            self._send_json(403, "Forbidden", {"status": "error", "error": "Web import is turned off on this server"})
            return
        source, title = str(body.get("source") or ""), str(body.get("title") or "")
        # The name is taken: ask before replacing it (replace names the file
        # the user agreed to replace; keep_both saves the new one beside it)
        replace = str(body.get("replace") or "")
        if not replace and not body.get("keep_both"):
            existing = app.library.existing_import_target(source, title)
            if existing:
                self._send_json(409, "Conflict", {"status": "exists", "name": existing})
                return
        ok, err = app.library.start_import(
            source,
            title=title,
            skip_enrichment=bool(body.get("skip_enrichment")),
            replace=replace,
        )
        if ok:
            self._send_json(200, "OK", {"status": "started"})
        else:
            self._send_json(409, "Conflict", {"status": "error", "error": err or "An import job is already in progress"})

    def _post_import_cancel(self, app: "TinyDVDWebServer", _body: dict) -> None:
        if not app.allow_import:
            self._send_json(403, "Forbidden", {"status": "error", "error": "Web import is turned off on this server"})
            return
        app.library.cancel_import()
        self._send_json(200, "OK", {"status": "cancelling"})

    def _post_play(self, app: "TinyDVDWebServer", body: dict) -> None:
        disc_id = sanitize_id(str(body.get("disc") or ""))
        item = app.library.find_by_id(disc_id)
        if not item or item.id != disc_id:
            self._send_error_text(404, "Movie Not Found")
            return
        sess = app.get_or_create_session(sanitize_id(str(body.get("session") or "")), item.path, item.id, item.title)
        if not sess:
            self._send_error_text(503, "Too Many Sessions")
            return
        sess.switch_movie(item.path, item.id, item.title)
        self._send_json(200, "OK", {"status": "ok", "session_id": sess.session_id})

    def _get_thumbnail(self, app: "TinyDVDWebServer", path: str, qs: dict) -> None:
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
            self._send_bytes(200, "OK", "image/png", png_data,
                             extra_headers={"Cache-Control": "private, max-age=86400"})
        else:
            self._send_error_text(404, "Thumbnail Not Found")

    def _get_media(self, app: "TinyDVDWebServer", path: str, qs: dict) -> None:
        """/api/media/NAME.m3u8 (HLS playlists) and /api/media/NAME.mp4 (their byte ranges)."""
        base_name, ext = os.path.splitext(path[len("/api/media/"):])
        if not MEDIA_NAME_RE.fullmatch(base_name + ".mp4") or ext not in (".mp4", ".m3u8"):
            self._send_error_text(400, "Invalid Media Name")
            return
        disc_id = sanitize_id(qs.get("disc", [""])[0])
        if ext == ".m3u8":
            body = app.playlist(disc_id, base_name, qs.get("part", [""])[0])
            if not body:
                self._send_error_text(404, "Media Stream Not Found")
                return
            self._send_bytes(200, "OK", "application/vnd.apple.mpegurl", body,
                             extra_headers={"Cache-Control": "no-cache"})
            return
        loc = app.locate_media(disc_id, base_name + ext)
        if not loc:
            self._send_error_text(404, "Media Stream Not Found")
            return
        self._serve_range_file(loc.path, self.headers.get("Range"), loc.offset, loc.size)

    def _serve_range_file(self, file_path: str, range_header: str | None,
                          base: int = 0, total: int | None = None) -> None:
        """Serve file_path[base:base+total] (the whole file by default), honouring a Range header."""
        if total is None:
            try:
                total = os.path.getsize(file_path) - base
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
                        end = min(int(e_part), total - 1) if e_part else (total - 1)
                except ValueError:
                    partial = False
                    start, end = 0, total - 1

        if partial and (start > end or start >= total):
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

                f.seek(base + start)
                rem = content_length
                while rem > 0:
                    chunk = f.read(min(CHUNK_SIZE, rem))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    rem -= len(chunk)
                self.wfile.flush()
        except OSError:
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
        is_dir, items, raw_target = app.library.get_snapshot()
        if is_dir:
            # Library: a new session waits for the viewer to pick a movie
            default_path = default_id = default_title = ""
        else:
            # A single disc: start playing it straight away
            default_path = items[0].path if items else raw_target
            default_id = items[0].id if items else disc_id_for_path(default_path)
            default_title = items[0].title if items else os.path.basename(default_path)

        sess = app.get_or_create_session(session_param, default_path, default_id, default_title,
                                         allow_no_disc=is_dir)
        if not sess:
            self._send_error_text(503, "Too Many Sessions" if default_path or is_dir else "No Disc Found")
            return

        self.close_connection = True
        sock = self.connection
        resp = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {compute_ws_accept_key(ws_key)}\r\n\r\n"
        ).encode("ascii")
        try:
            sock.sendall(resp)
        except OSError:
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
                    with sess.ws_lock:
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
        password: str,
        bind_addr: str = "127.0.0.1",
        port: int = 8080,
        announce: bool = True,
        listen: bool = True,
        allow_import: bool = False,
    ):
        self.core = core
        self.auth = WebAuth(password)
        self.allow_import = bool(allow_import)
        self.library = TinyDVDLibrary(target_path)
        self.bind_addr = bind_addr or "127.0.0.1"
        self.port = 8080 if port is None else int(port)

        self.sessions_lock = threading.Lock()
        self.sessions: dict[str, WebSession] = {}

        self.mp4_info: dict[tuple, dict] = {}
        self.playlist_lock = threading.Lock()

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
            host = "localhost" if self.bind_addr in ("127.0.0.1", "::1", "localhost") else self.bind_addr
            print("\n=======================================================")
            print("  TinyDVD Web Player")
            print(f"  URL: http://{host}:{self.port}")
            print(f"  {'Folder' if is_dir else 'Disc'}: {self.library.target_path}")
            print(f"  Mode: {'Library' if is_dir else 'Single disc'} ({cnt} movie{'' if cnt == 1 else 's'})")
            print(f"  Web import: {'on' if self.allow_import else 'off (start with --allow-web-import to turn on)'}")
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
        self, session_id: str | None, disc_path: str, disc_id: str, disc_title: str,
        allow_no_disc: bool = False,
    ) -> WebSession | None:
        """
        The named session, or a new one. A new one starts playing disc_path, or
        with allow_no_disc and no disc_path, waits for switch_movie(). None if
        there's no disc to play or there are too many sessions.
        """
        sid = sanitize_id(session_id or "")
        if not sid:
            sid = "s_" + uuid.uuid4().hex[:16]

        with self.sessions_lock:
            existing = self.sessions.get(sid)
            if existing:
                existing.last_active_time = time.time()
                return existing
            full = len(self.sessions) >= MAX_SESSIONS

        if not disc_path and not allow_no_disc:
            return None
        if full and not self._evict_idle_session():
            return None

        sess = WebSession(self.core, sid, disc_path, disc_id, disc_title)
        with self.sessions_lock:
            again = self.sessions.get(sid)
            if again:
                sess.destroy()
                return again
            self.sessions[sid] = sess
        print(f"[web_server] Started session {sid}" + (f" for {disc_title or disc_id}" if disc_path else ""),
              flush=True)
        return sess

    def _evict_idle_session(self) -> bool:
        """Free a slot by ending the least recently used session with no clients."""
        with self.sessions_lock:
            idle = [s for s in self.sessions.values() if s.client_count() == 0]
            if not idle:
                return False
            victim = min(idle, key=lambda s: s.last_active_time)
            self.sessions.pop(victim.session_id, None)
        victim.destroy()
        return True

    def _disc_path(self, disc_id: str) -> str:
        item = self.library.find_by_id(disc_id)
        if item:
            return item.path
        is_dir, items, raw_target = self.library.get_snapshot()
        return items[0].path if items else (raw_target if not is_dir else "")

    def locate_media(self, disc_id: str, name: str):
        """Where one of a disc's video or audio files is (a file, or a slice of a .tdvd)."""
        if not MEDIA_NAME_RE.fullmatch(name):
            return None
        loc = self.library.locate_video(self._disc_path(disc_id), name)
        # Packages are stored uncompressed, so every member is a byte range
        return loc if loc and loc.kind in ("file", "slice") else None

    def _media_info(self, loc) -> dict:
        """read_mp4 for a located file, remembered while the file is unchanged."""
        st = os.stat(loc.path)
        key = (os.path.realpath(loc.path), loc.offset, loc.size, st.st_mtime_ns)
        with self.playlist_lock:
            hit = self.mp4_info.get(key)
        if hit:
            return hit
        info = tinydvd_mp4.read_mp4(loc.path, loc.offset, loc.size)
        with self.playlist_lock:
            self.mp4_info[key] = info
            while len(self.mp4_info) > 256:
                self.mp4_info.pop(next(iter(self.mp4_info)))
        return info

    def playlist(self, disc_id: str, name: str, part: str = "") -> bytes | None:
        """
        An HLS playlist for one of a disc's videos.

        name is a video's base name: NAME_VIDEO gives the master playlist (the
        video, with each audio file as an audio track), and with part="video"
        the video's own segment list; NAME_AUDIO_n gives that track's. The
        segments are byte ranges of the packaged MP4s, served as they are.
        """
        quoted = urllib.parse.quote(disc_id)
        try:
            if name.endswith("_VIDEO") and part != "video":
                video = self.locate_media(disc_id, f"{name}.mp4")
                if not video:
                    return None
                info = self._media_info(video)
                members = tinydvd_archive.list_members(self._disc_path(disc_id))
                audio = []
                for i, audio_name in enumerate(tinydvd_mp4.audio_member_names(f"{name}.mp4", members)):
                    base = os.path.splitext(audio_name)[0]
                    audio.append((f"{base}.m3u8?disc={quoted}", f"Audio {i + 1}", members[audio_name]))
                body = tinydvd_mp4.build_master_playlist(f"{name}.m3u8?disc={quoted}&part=video",
                                                         info, video.size, audio)
                return body.encode("ascii")
            loc = self.locate_media(disc_id, f"{name}.mp4")
            if not loc:
                return None
            info = self._media_info(loc)
            body = tinydvd_mp4.build_media_playlist(info, f"{name}.mp4?disc={quoted}", HLS_SEGMENT_SECONDS)
            return body.encode("ascii")
        except (OSError, ValueError) as e:
            print(f"[web_server] Can't stream {name}: {e}", flush=True)
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
                print(f"[web_server] Ending idle session {sess.session_id} ({sess.disc_title})", flush=True)
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
            self.httpd.shutdown()
            self.httpd.server_close()

        with self.sessions_lock:
            all_sessions = list(self.sessions.values())
            self.sessions.clear()
        for sess in all_sessions:
            sess.destroy()

        self.library.destroy()

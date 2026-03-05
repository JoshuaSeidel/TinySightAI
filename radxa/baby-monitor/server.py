#!/usr/bin/env python3
"""
Baby Monitor HTTP API Server

Serves the web UI (index.html, style.css, app.js) and provides a REST API
that the phone browser uses to control the system.  Commands are forwarded to
the compositor via a TCP control channel on port 5290.

Port:       8080
µStreamer:  http://localhost:8082/stream  (MJPEG source)
Compositor: tcp://localhost:5290          (control commands)

Endpoints:
    GET  /                 — serve index.html
    GET  /style.css        — serve stylesheet
    GET  /app.js           — serve JavaScript
    GET  /stream           — reverse-proxy µStreamer MJPEG stream
    GET  /api/status       — JSON status from compositor
    POST /api/zoom         — {"action": "in" | "out" | "reset"}
    POST /api/ir           — {"state": "on" | "off" | "auto"}
    POST /api/mode         — {"mode": "full_aa" | "full_carplay" | ...}
"""

import json
import os
import socket
import threading
import http.client
import urllib.request
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SERVER_PORT       = 8080
COMPOSITOR_HOST   = "127.0.0.1"
COMPOSITOR_PORT   = 5290
USTREAMER_HOST    = "127.0.0.1"
USTREAMER_PORT    = 8082
STATIC_DIR        = Path(__file__).parent

CONTROL_TIMEOUT   = 3.0   # seconds

VALID_MODES = {
    "full_aa",
    "full_carplay",
    "full_camera",
    "split_aa_cam",
    "split_cp_cam",
}

# ---------------------------------------------------------------------------
# Compositor control channel
# ---------------------------------------------------------------------------

_compositor_lock = threading.Lock()


def _send_compositor_command(command: str) -> str:
    """
    Send a single-line command to the compositor control TCP socket and
    return the first line of the response.

    The compositor protocol is plain-text, one command per connection:
        > MODE split_aa_cam\\n
        < OK\\n

    Returns the response string (stripped), or raises RuntimeError on failure.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(CONTROL_TIMEOUT)
        try:
            sock.connect((COMPOSITOR_HOST, COMPOSITOR_PORT))
        except OSError as exc:
            raise RuntimeError(f"Cannot connect to compositor: {exc}") from exc

        sock.sendall((command.strip() + "\n").encode())

        # Read response (up to 4 KB)
        chunks = []
        try:
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
                # Stop after first newline
                if b"\n" in chunk:
                    break
        except socket.timeout:
            pass

    response = b"".join(chunks).decode(errors="replace").strip()
    return response


def compositor_command(command: str) -> str:
    """Thread-safe wrapper around _send_compositor_command."""
    with _compositor_lock:
        return _send_compositor_command(command)


def get_status_from_compositor() -> dict:
    """
    Send STATUS command to compositor and parse response.

    Expected compositor response (JSON or key=value):
        {"mode": "full_aa", "zoom": 1, "ir": "auto", "fps": 30, "camera": true}

    Returns a dict with safe defaults if compositor is unreachable.
    """
    defaults = {
        "mode":   "full_aa",
        "zoom":   1,
        "ir":     "auto",
        "fps":    0,
        "camera": False,
    }
    try:
        raw = compositor_command("STATUS")
        data = json.loads(raw)
        defaults.update(data)
    except (RuntimeError, json.JSONDecodeError, OSError):
        pass
    return defaults

# ---------------------------------------------------------------------------
# MJPEG stream proxy
# ---------------------------------------------------------------------------

MJPEG_PROXY_CHUNK = 4096


def _proxy_mjpeg(rfile_write, request_handler):
    """
    Open a connection to µStreamer and pipe the MJPEG stream to the client.
    Runs until the client disconnects or µStreamer is unreachable.
    """
    conn = http.client.HTTPConnection(USTREAMER_HOST, USTREAMER_PORT,
                                       timeout=5)
    try:
        conn.request("GET", "/stream")
        resp = conn.getresponse()

        # Forward status + headers to client
        request_handler.send_response(resp.status)
        for header, value in resp.getheaders():
            # Skip hop-by-hop headers that must not be forwarded
            if header.lower() in ("transfer-encoding", "connection",
                                   "keep-alive", "te", "trailers",
                                   "upgrade"):
                continue
            request_handler.send_header(header, value)
        request_handler.end_headers()

        # Pipe data until either side closes
        while True:
            chunk = resp.read(MJPEG_PROXY_CHUNK)
            if not chunk:
                break
            try:
                request_handler.wfile.write(chunk)
                request_handler.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                break

    except (OSError, http.client.HTTPException) as exc:
        # µStreamer not running — send a simple error
        try:
            request_handler.send_error(502, f"µStreamer unavailable: {exc}")
        except Exception:
            pass
    finally:
        conn.close()

# ---------------------------------------------------------------------------
# HTTP request handler
# ---------------------------------------------------------------------------


class BabyMonitorHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        # Use structured logging format
        print(f"[baby-monitor] {self.address_string()} - {fmt % args}")

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _send_json(self, data: dict, status: int = 200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _send_error_json(self, message: str, status: int = 400):
        self._send_json({"error": message}, status)

    def _read_json_body(self) -> dict:
        length = int(self.headers.get("Content-Length", 0))
        if length == 0:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {}

    def _serve_static(self, filename: str):
        path = STATIC_DIR / filename
        if not path.exists():
            self._send_error_json(f"File not found: {filename}", 404)
            return

        ext_map = {
            ".html": "text/html; charset=utf-8",
            ".css":  "text/css; charset=utf-8",
            ".js":   "application/javascript; charset=utf-8",
        }
        content_type = ext_map.get(path.suffix, "application/octet-stream")

        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # ------------------------------------------------------------------
    # GET
    # ------------------------------------------------------------------

    def do_GET(self):
        path = self.path.split("?")[0]  # strip query string

        if path == "/" or path == "/index.html":
            self._serve_static("index.html")

        elif path == "/style.css":
            self._serve_static("style.css")

        elif path == "/app.js":
            self._serve_static("app.js")

        elif path == "/stream":
            # Reverse-proxy the µStreamer MJPEG stream
            _proxy_mjpeg(self.wfile.write, self)

        elif path == "/api/status":
            status = get_status_from_compositor()
            self._send_json(status)

        else:
            self._send_error_json("Not found", 404)

    # ------------------------------------------------------------------
    # POST
    # ------------------------------------------------------------------

    def do_POST(self):
        path = self.path.split("?")[0]
        body = self._read_json_body()

        if path == "/api/zoom":
            action = body.get("action", "")
            if action not in ("in", "out", "reset"):
                self._send_error_json(
                    f"Invalid zoom action '{action}'. "
                    "Use 'in', 'out', or 'reset'."
                )
                return
            try:
                resp = compositor_command(f"ZOOM {action.upper()}")
                self._send_json({"ok": True, "response": resp})
            except RuntimeError as exc:
                self._send_error_json(str(exc), 503)

        elif path == "/api/ir":
            state = body.get("state", "")
            if state not in ("on", "off", "auto"):
                self._send_error_json(
                    f"Invalid IR state '{state}'. Use 'on', 'off', or 'auto'."
                )
                return
            try:
                resp = compositor_command(f"IR {state.upper()}")
                self._send_json({"ok": True, "response": resp})
            except RuntimeError as exc:
                self._send_error_json(str(exc), 503)

        elif path == "/api/mode":
            mode = body.get("mode", "")
            if mode not in VALID_MODES:
                self._send_error_json(
                    f"Invalid mode '{mode}'. "
                    f"Valid modes: {sorted(VALID_MODES)}"
                )
                return
            try:
                resp = compositor_command(f"MODE {mode}")
                self._send_json({"ok": True, "response": resp})
            except RuntimeError as exc:
                self._send_error_json(str(exc), 503)

        else:
            self._send_error_json("Not found", 404)

    # ------------------------------------------------------------------
    # OPTIONS (CORS preflight)
    # ------------------------------------------------------------------

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods",
                         "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main():
    server = HTTPServer(("0.0.0.0", SERVER_PORT), BabyMonitorHandler)
    print(f"Baby Monitor server running on port {SERVER_PORT}")
    print(f"Static files:   {STATIC_DIR}")
    print(f"Compositor:     {COMPOSITOR_HOST}:{COMPOSITOR_PORT}")
    print(f"µStreamer:       http://{USTREAMER_HOST}:{USTREAMER_PORT}/stream")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Baby Monitor server stopped")
        server.server_close()


if __name__ == "__main__":
    main()

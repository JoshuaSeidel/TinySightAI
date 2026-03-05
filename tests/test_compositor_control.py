#!/usr/bin/env python3
"""
test_compositor_control.py — Compositor Control Channel Protocol Tests

Tests the plain-text TCP control channel on port 5290.

Protocol:
    One command per connection (or multiple commands on a persistent
    connection — both patterns are tested).
    Commands are newline-terminated ASCII strings.
    Server responds with a newline-terminated ASCII string.

Supported commands:
    STATUS          — returns JSON: {"mode":..., "zoom":N, "ir":..., "fps":N, "camera":...}
    MODE <name>     — set display mode; returns "OK" or "ERROR: ..."
    ZOOM IN         — zoom in one step; returns "OK" or "ERROR: ..."
    ZOOM OUT        — zoom out one step
    ZOOM RESET      — reset zoom to 1.0x
    IR ON           — force IR LEDs on
    IR OFF          — force IR LEDs off
    IR AUTO         — set IR to auto mode

Valid modes:
    full_aa, full_carplay, full_camera, split_aa_cam, split_cp_cam
"""

import json
import os
import socket
import threading
import time
import unittest

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

CTRL_HOST = os.environ.get("COMPOSITOR_HOST", "127.0.0.1")
CTRL_PORT = int(os.environ.get("COMPOSITOR_CTRL_PORT", "5290"))
TIMEOUT   = 3.0

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def send_command(command: str, host: str = CTRL_HOST,
                  port: int = CTRL_PORT) -> str:
    """
    Open a connection, send command, read response, close.
    Returns the response string (stripped of whitespace).
    """
    with socket.create_connection((host, port), timeout=TIMEOUT) as s:
        s.sendall((command.strip() + "\n").encode())
        data = b""
        s.settimeout(TIMEOUT)
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
                if b"\n" in chunk:
                    break
        except socket.timeout:
            pass
    return data.decode(errors="replace").strip()


# ---------------------------------------------------------------------------
# Stub server for offline testing
# ---------------------------------------------------------------------------

def _stub_compositor_handle(conn: socket.socket, state: dict,
                              lock: threading.Lock) -> None:
    """Handle one connection to the stub compositor server."""
    conn.settimeout(0.5)
    buf = b""
    valid_modes = {
        "full_aa", "full_carplay", "full_camera",
        "split_aa_cam", "split_cp_cam",
    }
    try:
        while True:
            try:
                chunk = conn.recv(256)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.decode().strip()
                parts = cmd.upper().split()
                if not parts:
                    response = "ERROR: empty command"
                elif parts[0] == "STATUS":
                    with lock:
                        response = json.dumps(state)
                elif parts[0] == "MODE":
                    if len(parts) < 2:
                        response = "ERROR: missing mode argument"
                    else:
                        mode = parts[1].lower()
                        if mode not in valid_modes:
                            response = f"ERROR: unknown mode '{mode}'"
                        else:
                            with lock:
                                state["mode"] = mode
                            response = "OK"
                elif parts[0] == "ZOOM":
                    if len(parts) < 2:
                        response = "ERROR: missing zoom argument"
                    else:
                        action = parts[1]
                        with lock:
                            if action == "IN":
                                state["zoom"] = min(8, state["zoom"] + 1)
                                response = "OK"
                            elif action == "OUT":
                                state["zoom"] = max(1, state["zoom"] - 1)
                                response = "OK"
                            elif action == "RESET":
                                state["zoom"] = 1
                                response = "OK"
                            else:
                                response = f"ERROR: unknown zoom action '{action}'"
                elif parts[0] == "IR":
                    if len(parts) < 2:
                        response = "ERROR: missing IR argument"
                    else:
                        ir_state = parts[1].lower()
                        if ir_state not in ("on", "off", "auto"):
                            response = f"ERROR: unknown IR state '{ir_state}'"
                        else:
                            with lock:
                                state["ir"] = ir_state
                            response = "OK"
                else:
                    response = f"ERROR: unknown command '{parts[0]}'"
                conn.sendall((response + "\n").encode())
    except OSError:
        pass
    finally:
        conn.close()


class _StubCompositorServer:
    """
    In-process stub compositor control server.
    Implements the expected protocol so tests can run offline.
    Does NOT inherit from threading.Thread (avoids Python 3.13 _ThreadHandle
    issue with bound methods on Thread subclasses).
    """

    def __init__(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self.host, self.port = self._sock.getsockname()
        self._sock.listen(10)
        self._stop = threading.Event()

        self._state = {
            "mode":   "full_aa",
            "zoom":   1,
            "ir":     "auto",
            "fps":    30,
            "camera": True,
        }
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        import selectors
        sel = selectors.DefaultSelector()
        self._sock.setblocking(False)
        sel.register(self._sock, selectors.EVENT_READ, data="listen")
        while not self._stop.is_set():
            events = sel.select(timeout=0.2)
            for key, _ in events:
                if key.data == "listen":
                    try:
                        conn, _ = self._sock.accept()
                    except OSError:
                        continue
                    conn.setblocking(True)
                    _stub_compositor_handle(conn, self._state, self._lock)
        sel.close()

    def stop(self):
        self._stop.set()
        self._sock.close()


# ---------------------------------------------------------------------------
# Base class: try real compositor, fall back to stub
# ---------------------------------------------------------------------------

class ControlTestBase(unittest.TestCase):

    _stub: _StubCompositorServer = None
    _use_stub: bool = False

    @classmethod
    def setUpClass(cls):
        try:
            s = socket.create_connection((CTRL_HOST, CTRL_PORT), timeout=1.0)
            s.close()
            cls._use_stub = False
        except OSError:
            # _StubCompositorServer starts its thread in __init__
            cls._stub = _StubCompositorServer()
            cls._use_stub = True

    @classmethod
    def tearDownClass(cls):
        if cls._stub:
            cls._stub.stop()

    def cmd(self, command: str) -> str:
        if self._use_stub:
            return send_command(command, self._stub.host, self._stub.port)
        return send_command(command)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestStatusCommand(ControlTestBase):

    def test_status_is_valid_json(self):
        raw = self.cmd("STATUS")
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            self.fail(f"STATUS response is not valid JSON: {raw!r}")
        self.assertIsInstance(data, dict)

    def test_status_has_required_keys(self):
        data = json.loads(self.cmd("STATUS"))
        for key in ("mode", "zoom", "ir", "fps", "camera"):
            self.assertIn(key, data, f"Missing key '{key}' in STATUS response")

    def test_status_mode_is_string(self):
        data = json.loads(self.cmd("STATUS"))
        self.assertIsInstance(data["mode"], str)

    def test_status_zoom_is_int(self):
        data = json.loads(self.cmd("STATUS"))
        self.assertIsInstance(data["zoom"], int)
        self.assertGreaterEqual(data["zoom"], 1)

    def test_status_ir_is_valid(self):
        data = json.loads(self.cmd("STATUS"))
        self.assertIn(data["ir"], ("on", "off", "auto"))

    def test_status_camera_is_bool(self):
        data = json.loads(self.cmd("STATUS"))
        self.assertIsInstance(data["camera"], bool)


class TestModeCommand(ControlTestBase):

    VALID_MODES = [
        "full_aa", "full_carplay", "full_camera",
        "split_aa_cam", "split_cp_cam",
    ]

    def test_set_each_valid_mode(self):
        for mode in self.VALID_MODES:
            with self.subTest(mode=mode):
                resp = self.cmd(f"MODE {mode}")
                self.assertEqual(resp, "OK",
                                  f"MODE {mode} returned: {resp!r}")
                # Verify STATUS reflects the change
                data = json.loads(self.cmd("STATUS"))
                self.assertEqual(data["mode"], mode)

    def test_mode_invalid(self):
        resp = self.cmd("MODE banana")
        self.assertIn("ERROR", resp.upper())

    def test_mode_missing_argument(self):
        resp = self.cmd("MODE")
        self.assertIn("ERROR", resp.upper())


class TestZoomCommand(ControlTestBase):

    def setUp(self):
        # Reset zoom before each test
        self.cmd("ZOOM RESET")

    def test_zoom_in(self):
        before = json.loads(self.cmd("STATUS"))["zoom"]
        resp = self.cmd("ZOOM IN")
        self.assertEqual(resp, "OK")
        after = json.loads(self.cmd("STATUS"))["zoom"]
        self.assertGreater(after, before)

    def test_zoom_out(self):
        # First zoom in so we have room to zoom out
        self.cmd("ZOOM IN")
        before = json.loads(self.cmd("STATUS"))["zoom"]
        resp = self.cmd("ZOOM OUT")
        self.assertEqual(resp, "OK")
        after = json.loads(self.cmd("STATUS"))["zoom"]
        self.assertLess(after, before)

    def test_zoom_reset(self):
        self.cmd("ZOOM IN")
        self.cmd("ZOOM IN")
        resp = self.cmd("ZOOM RESET")
        self.assertEqual(resp, "OK")
        data = json.loads(self.cmd("STATUS"))
        self.assertEqual(data["zoom"], 1)

    def test_zoom_invalid_action(self):
        resp = self.cmd("ZOOM MAXIMUM")
        self.assertIn("ERROR", resp.upper())

    def test_zoom_missing_argument(self):
        resp = self.cmd("ZOOM")
        self.assertIn("ERROR", resp.upper())


class TestIRCommand(ControlTestBase):

    def test_ir_on(self):
        resp = self.cmd("IR ON")
        self.assertEqual(resp, "OK")
        data = json.loads(self.cmd("STATUS"))
        self.assertEqual(data["ir"], "on")

    def test_ir_off(self):
        resp = self.cmd("IR OFF")
        self.assertEqual(resp, "OK")
        data = json.loads(self.cmd("STATUS"))
        self.assertEqual(data["ir"], "off")

    def test_ir_auto(self):
        resp = self.cmd("IR AUTO")
        self.assertEqual(resp, "OK")
        data = json.loads(self.cmd("STATUS"))
        self.assertEqual(data["ir"], "auto")

    def test_ir_invalid(self):
        resp = self.cmd("IR MAYBE")
        self.assertIn("ERROR", resp.upper())

    def test_ir_missing_argument(self):
        resp = self.cmd("IR")
        self.assertIn("ERROR", resp.upper())


class TestUnknownCommand(ControlTestBase):

    def test_unknown_command(self):
        resp = self.cmd("FROBNICATOR")
        self.assertIn("ERROR", resp.upper())


class TestRapidCommandSequence(ControlTestBase):

    def test_rapid_status_calls(self):
        """Fire 20 STATUS commands quickly; all should return valid JSON."""
        for i in range(20):
            with self.subTest(i=i):
                raw = self.cmd("STATUS")
                try:
                    json.loads(raw)
                except json.JSONDecodeError:
                    self.fail(f"Iteration {i}: STATUS returned invalid JSON: {raw!r}")

    def test_rapid_mode_switches(self):
        modes = [
            "full_aa", "split_aa_cam", "full_camera",
            "split_cp_cam", "full_carplay",
        ]
        for mode in modes * 3:
            resp = self.cmd(f"MODE {mode}")
            self.assertEqual(resp, "OK", f"Failed setting mode={mode}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)

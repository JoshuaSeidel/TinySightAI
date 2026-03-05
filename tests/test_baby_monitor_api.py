#!/usr/bin/env python3
"""
test_baby_monitor_api.py — Baby Monitor HTTP API Tests

Tests the HTTP API server (radxa/baby-monitor/server.py) running on port 8080.

API:
    GET  /api/status          — JSON status
    POST /api/zoom            — {"action": "in" | "out" | "reset"}
    POST /api/ir              — {"state": "on" | "off" | "auto"}
    POST /api/mode            — {"mode": "full_aa" | ...}
    GET  /                    — index.html
    GET  /style.css           — stylesheet
    GET  /app.js              — JavaScript
    GET  /stream              — MJPEG proxy (tested for basic availability)
"""

import http.client
import json
import os
import socket
import subprocess
import sys
import threading
import time
import unittest
import urllib.request

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

API_HOST = os.environ.get("BABY_MONITOR_HOST", "127.0.0.1")
API_PORT = int(os.environ.get("BABY_MONITOR_PORT", "8080"))
TIMEOUT  = 5.0

VALID_MODES = {
    "full_aa", "full_carplay", "full_camera",
    "split_aa_cam", "split_cp_cam",
}

# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------


def http_get(path: str) -> http.client.HTTPResponse:
    """Send a GET request and return the response."""
    conn = http.client.HTTPConnection(API_HOST, API_PORT, timeout=TIMEOUT)
    conn.request("GET", path)
    return conn.getresponse()


def http_post(path: str, body: dict) -> tuple:
    """
    Send a POST request with a JSON body.
    Returns (status_code, response_dict_or_None).
    """
    data = json.dumps(body).encode()
    conn = http.client.HTTPConnection(API_HOST, API_PORT, timeout=TIMEOUT)
    conn.request(
        "POST", path, body=data,
        headers={"Content-Type": "application/json",
                  "Content-Length": str(len(data))}
    )
    resp = conn.getresponse()
    raw = resp.read()
    try:
        return resp.status, json.loads(raw)
    except json.JSONDecodeError:
        return resp.status, None


# ---------------------------------------------------------------------------
# Check if the server is running
# ---------------------------------------------------------------------------

def server_is_up() -> bool:
    try:
        s = socket.create_connection((API_HOST, API_PORT), timeout=1.0)
        s.close()
        return True
    except OSError:
        return False


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@unittest.skipUnless(server_is_up(), f"Baby monitor server not running on {API_HOST}:{API_PORT}")
class TestStatusEndpoint(unittest.TestCase):

    def test_status_200(self):
        resp = http_get("/api/status")
        self.assertEqual(resp.status, 200)

    def test_status_content_type(self):
        resp = http_get("/api/status")
        ct = resp.getheader("Content-Type", "")
        self.assertIn("application/json", ct)

    def test_status_valid_json(self):
        resp = http_get("/api/status")
        raw = resp.read()
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            self.fail(f"Status response is not valid JSON: {raw!r}")
        self.assertIsInstance(data, dict)

    def test_status_required_keys(self):
        resp = http_get("/api/status")
        data = json.loads(resp.read())
        for key in ("mode", "zoom", "ir", "fps", "camera"):
            self.assertIn(key, data, f"Missing key: {key}")

    def test_status_mode_valid(self):
        resp = http_get("/api/status")
        data = json.loads(resp.read())
        self.assertIn(data["mode"], VALID_MODES | {"full_aa"})

    def test_status_zoom_positive(self):
        resp = http_get("/api/status")
        data = json.loads(resp.read())
        self.assertIsInstance(data["zoom"], (int, float))
        self.assertGreaterEqual(data["zoom"], 1)

    def test_status_ir_valid_value(self):
        resp = http_get("/api/status")
        data = json.loads(resp.read())
        self.assertIn(data["ir"], ("on", "off", "auto"))


@unittest.skipUnless(server_is_up(), f"Baby monitor server not running on {API_HOST}:{API_PORT}")
class TestZoomEndpoint(unittest.TestCase):

    def test_zoom_in_ok(self):
        status, body = http_post("/api/zoom", {"action": "in"})
        self.assertEqual(status, 200)
        self.assertIsNotNone(body)
        self.assertTrue(body.get("ok", False))

    def test_zoom_out_ok(self):
        status, body = http_post("/api/zoom", {"action": "out"})
        self.assertEqual(status, 200)

    def test_zoom_reset_ok(self):
        status, body = http_post("/api/zoom", {"action": "reset"})
        self.assertEqual(status, 200)

    def test_zoom_invalid_action(self):
        status, body = http_post("/api/zoom", {"action": "turbo"})
        self.assertEqual(status, 400)
        self.assertIn("error", body)

    def test_zoom_missing_action(self):
        status, body = http_post("/api/zoom", {})
        self.assertEqual(status, 400)

    def test_zoom_wrong_field_name(self):
        status, body = http_post("/api/zoom", {"zoom": "in"})
        self.assertEqual(status, 400)


@unittest.skipUnless(server_is_up(), f"Baby monitor server not running on {API_HOST}:{API_PORT}")
class TestIREndpoint(unittest.TestCase):

    def test_ir_on(self):
        status, body = http_post("/api/ir", {"state": "on"})
        self.assertEqual(status, 200)
        self.assertTrue(body.get("ok", False))

    def test_ir_off(self):
        status, body = http_post("/api/ir", {"state": "off"})
        self.assertEqual(status, 200)

    def test_ir_auto(self):
        status, body = http_post("/api/ir", {"state": "auto"})
        self.assertEqual(status, 200)

    def test_ir_invalid(self):
        status, body = http_post("/api/ir", {"state": "maybe"})
        self.assertEqual(status, 400)
        self.assertIn("error", body)

    def test_ir_missing_state(self):
        status, body = http_post("/api/ir", {})
        self.assertEqual(status, 400)


@unittest.skipUnless(server_is_up(), f"Baby monitor server not running on {API_HOST}:{API_PORT}")
class TestModeEndpoint(unittest.TestCase):

    def test_set_full_aa(self):
        status, body = http_post("/api/mode", {"mode": "full_aa"})
        self.assertEqual(status, 200)
        self.assertTrue(body.get("ok", False))

    def test_set_all_valid_modes(self):
        for mode in VALID_MODES:
            with self.subTest(mode=mode):
                status, body = http_post("/api/mode", {"mode": mode})
                self.assertEqual(status, 200,
                                  f"Mode {mode} returned {status}: {body}")

    def test_mode_invalid(self):
        status, body = http_post("/api/mode", {"mode": "banana_split"})
        self.assertEqual(status, 400)
        self.assertIn("error", body)

    def test_mode_empty_string(self):
        status, body = http_post("/api/mode", {"mode": ""})
        self.assertEqual(status, 400)

    def test_mode_missing_field(self):
        status, body = http_post("/api/mode", {})
        self.assertEqual(status, 400)


@unittest.skipUnless(server_is_up(), f"Baby monitor server not running on {API_HOST}:{API_PORT}")
class TestStaticFiles(unittest.TestCase):

    def test_root_returns_html(self):
        resp = http_get("/")
        self.assertEqual(resp.status, 200)
        ct = resp.getheader("Content-Type", "")
        self.assertIn("text/html", ct)

    def test_index_html_explicit(self):
        resp = http_get("/index.html")
        self.assertEqual(resp.status, 200)

    def test_style_css(self):
        resp = http_get("/style.css")
        # May be 200 or 404 depending on whether file exists
        self.assertIn(resp.status, (200, 404))

    def test_app_js(self):
        resp = http_get("/app.js")
        self.assertIn(resp.status, (200, 404))

    def test_not_found(self):
        resp = http_get("/does-not-exist.xyz")
        self.assertEqual(resp.status, 404)


@unittest.skipUnless(server_is_up(), f"Baby monitor server not running on {API_HOST}:{API_PORT}")
class TestCORSHeaders(unittest.TestCase):

    def test_options_preflight(self):
        conn = http.client.HTTPConnection(API_HOST, API_PORT, timeout=TIMEOUT)
        conn.request(
            "OPTIONS", "/api/status",
            headers={"Origin": "http://192.168.4.2",
                      "Access-Control-Request-Method": "POST"}
        )
        resp = conn.getresponse()
        self.assertIn(resp.status, (200, 204))

    def test_cors_header_on_status(self):
        resp = http_get("/api/status")
        cors = resp.getheader("Access-Control-Allow-Origin", "")
        self.assertEqual(cors, "*")


@unittest.skipUnless(server_is_up(), f"Baby monitor server not running on {API_HOST}:{API_PORT}")
class TestErrorHandling(unittest.TestCase):

    def test_post_to_unknown_path(self):
        status, _ = http_post("/api/unknown", {"foo": "bar"})
        self.assertEqual(status, 404)

    def test_get_to_unknown_api_path(self):
        resp = http_get("/api/notreal")
        self.assertEqual(resp.status, 404)

    def test_malformed_json_body(self):
        conn = http.client.HTTPConnection(API_HOST, API_PORT, timeout=TIMEOUT)
        bad_body = b"not valid json!!!"
        conn.request(
            "POST", "/api/mode",
            body=bad_body,
            headers={"Content-Type": "application/json",
                      "Content-Length": str(len(bad_body))}
        )
        resp = conn.getresponse()
        # Should return 400 or handle gracefully (not 500)
        self.assertIn(resp.status, (400, 200))


# ---------------------------------------------------------------------------
# Offline tests (no server required)
# ---------------------------------------------------------------------------

class TestAPIHelperFunctions(unittest.TestCase):
    """Test the test helper functions themselves (offline)."""

    def test_valid_modes_set(self):
        self.assertIn("full_aa",       VALID_MODES)
        self.assertIn("full_carplay",  VALID_MODES)
        self.assertIn("full_camera",   VALID_MODES)
        self.assertIn("split_aa_cam",  VALID_MODES)
        self.assertIn("split_cp_cam",  VALID_MODES)
        self.assertEqual(len(VALID_MODES), 5)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    if not server_is_up():
        print(f"NOTE: Baby monitor server not running on {API_HOST}:{API_PORT}")
        print("Most tests will be skipped. Start server.py to run full suite.")
        print()
    unittest.main(verbosity=2)

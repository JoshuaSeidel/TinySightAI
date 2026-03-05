#!/usr/bin/env python3
"""
test_ota_server.py — OTA Server Endpoint Tests

Tests the OTA server (radxa/ota-server/ota_server.py) running on port 8081.

Endpoints under test:
    GET  /api/ota/status           — existing endpoint: current versions
    GET  /dongle/latest            — NEW: JSON version info for T-Dongle
    GET  /dongle/firmware.bin      — NEW: dongle firmware binary
    GET  /brain/latest             — NEW: JSON version info for Radxa

These tests can run in two modes:
    1. Live server mode: connect to the actual OTA server on port 8081.
    2. Offline stub mode: spin up an in-process stub and test against it.
"""

import hashlib
import http.client
import json
import os
import socket
import struct
import threading
import time
import unittest
from http.server import HTTPServer, BaseHTTPRequestHandler
from io import BytesIO

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

OTA_HOST = os.environ.get("OTA_SERVER_HOST", "127.0.0.1")
OTA_PORT = int(os.environ.get("OTA_SERVER_PORT", "8081"))
TIMEOUT  = 5.0

# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------


def http_get(path: str, host: str = OTA_HOST,
              port: int = OTA_PORT) -> tuple:
    """
    Send a GET request.
    Returns (status, headers_dict, body_bytes).
    """
    conn = http.client.HTTPConnection(host, port, timeout=TIMEOUT)
    conn.request("GET", path)
    resp = conn.getresponse()
    body = resp.read()
    headers = {k.lower(): v for k, v in resp.getheaders()}
    return resp.status, headers, body


# ---------------------------------------------------------------------------
# Check if the OTA server is running
# ---------------------------------------------------------------------------

def server_is_up(host: str = OTA_HOST, port: int = OTA_PORT) -> bool:
    try:
        s = socket.create_connection((host, port), timeout=1.0)
        s.close()
        return True
    except OSError:
        return False


# ---------------------------------------------------------------------------
# In-process stub OTA server
# ---------------------------------------------------------------------------

STUB_DONGLE_VERSION = "1.2.3"
STUB_BRAIN_VERSION  = "1.0.1"

# Fake firmware binary (not a valid ESP32 image, just for transport testing)
STUB_FIRMWARE_BYTES = b"\xE9" + b"\x00" * 7 + b"\x55\xAA" + b"\x00" * 246
STUB_FIRMWARE_SHA256 = hashlib.sha256(STUB_FIRMWARE_BYTES).hexdigest()


class _StubOTAHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        pass

    def _json(self, data: dict, status: int = 200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path.split("?")[0]

        if p == "/api/ota/status":
            self._json({
                "brain_version":  STUB_BRAIN_VERSION,
                "dongle_version": STUB_DONGLE_VERSION,
            })

        elif p == "/dongle/latest":
            self._json({
                "version": STUB_DONGLE_VERSION,
                "size":    len(STUB_FIRMWARE_BYTES),
                "sha256":  STUB_FIRMWARE_SHA256,
                "url":     "/dongle/firmware.bin",
            })

        elif p == "/dongle/firmware.bin":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(STUB_FIRMWARE_BYTES)))
            self.end_headers()
            self.wfile.write(STUB_FIRMWARE_BYTES)

        elif p == "/brain/latest":
            self._json({
                "version": STUB_BRAIN_VERSION,
                "size":    1024,
                "sha256":  "a" * 64,
                "url":     "/brain/firmware.tar.gz",
            })

        else:
            self._json({"error": "Not found"}, 404)


class _StubOTAServer(threading.Thread):

    def __init__(self):
        super().__init__(daemon=True)
        self._server = HTTPServer(("127.0.0.1", 0), _StubOTAHandler)
        self.host = "127.0.0.1"
        self.port = self._server.server_address[1]
        self._stop = threading.Event()

    def run(self):
        self._server.timeout = 0.2
        while not self._stop.is_set():
            self._server.handle_request()

    def stop(self):
        self._stop.set()
        self._server.server_close()


# ---------------------------------------------------------------------------
# Base test class
# ---------------------------------------------------------------------------

class OTATestBase(unittest.TestCase):

    _stub: _StubOTAServer = None
    _use_stub: bool = False
    _host: str = OTA_HOST
    _port: int = OTA_PORT

    @classmethod
    def setUpClass(cls):
        if server_is_up():
            cls._use_stub = False
        else:
            cls._stub = _StubOTAServer()
            cls._stub.start()
            cls._use_stub = True
            cls._host = cls._stub.host
            cls._port = cls._stub.port
            time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        if cls._stub:
            cls._stub.stop()

    def get(self, path: str) -> tuple:
        return http_get(path, self._host, self._port)


# ---------------------------------------------------------------------------
# Tests — existing endpoints
# ---------------------------------------------------------------------------

class TestExistingStatusEndpoint(OTATestBase):

    def test_status_200(self):
        status, _, _ = self.get("/api/ota/status")
        self.assertEqual(status, 200)

    def test_status_valid_json(self):
        status, headers, body = self.get("/api/ota/status")
        self.assertEqual(status, 200)
        try:
            data = json.loads(body)
        except json.JSONDecodeError:
            self.fail(f"Status response not JSON: {body!r}")
        self.assertIsInstance(data, dict)

    def test_status_has_version_keys(self):
        _, _, body = self.get("/api/ota/status")
        data = json.loads(body)
        self.assertIn("dongle_version", data)
        self.assertIn("brain_version", data)

    def test_status_version_strings_are_strings(self):
        _, _, body = self.get("/api/ota/status")
        data = json.loads(body)
        self.assertIsInstance(data["dongle_version"], str)
        self.assertIsInstance(data["brain_version"],  str)


# ---------------------------------------------------------------------------
# Tests — /dongle/latest
# ---------------------------------------------------------------------------

class TestDongleLatestEndpoint(OTATestBase):

    def test_dongle_latest_200(self):
        status, _, _ = self.get("/dongle/latest")
        self.assertEqual(status, 200)

    def test_dongle_latest_json_content_type(self):
        _, headers, _ = self.get("/dongle/latest")
        ct = headers.get("content-type", "")
        self.assertIn("application/json", ct)

    def test_dongle_latest_valid_json(self):
        _, _, body = self.get("/dongle/latest")
        try:
            data = json.loads(body)
        except json.JSONDecodeError:
            self.fail(f"/dongle/latest not valid JSON: {body!r}")
        self.assertIsInstance(data, dict)

    def test_dongle_latest_has_required_fields(self):
        _, _, body = self.get("/dongle/latest")
        data = json.loads(body)
        for field in ("version", "sha256", "url"):
            self.assertIn(field, data, f"Missing field: {field}")

    def test_dongle_latest_version_format(self):
        _, _, body = self.get("/dongle/latest")
        data = json.loads(body)
        version = data["version"]
        # Should be in MAJOR.MINOR.PATCH format
        parts = version.split(".")
        self.assertGreaterEqual(len(parts), 2,
                                 f"Version '{version}' has fewer than 2 parts")
        for part in parts:
            self.assertTrue(part.isdigit(),
                             f"Version part '{part}' is not numeric")

    def test_dongle_latest_sha256_format(self):
        _, _, body = self.get("/dongle/latest")
        data = json.loads(body)
        sha = data.get("sha256", "")
        # SHA-256 hex digest = 64 hex chars
        self.assertEqual(len(sha), 64,
                          f"SHA-256 length {len(sha)} != 64: {sha!r}")
        self.assertTrue(all(c in "0123456789abcdefABCDEF" for c in sha),
                         f"SHA-256 contains non-hex chars: {sha!r}")

    def test_dongle_latest_size_if_present(self):
        _, _, body = self.get("/dongle/latest")
        data = json.loads(body)
        if "size" in data:
            self.assertIsInstance(data["size"], int)
            self.assertGreater(data["size"], 0)

    def test_dongle_latest_url_field(self):
        _, _, body = self.get("/dongle/latest")
        data = json.loads(body)
        url = data.get("url", "")
        self.assertIsInstance(url, str)
        self.assertTrue(url.startswith("/"),
                         f"URL should be a path starting with /: {url!r}")


# ---------------------------------------------------------------------------
# Tests — /dongle/firmware.bin
# ---------------------------------------------------------------------------

class TestDongleFirmwareEndpoint(OTATestBase):

    def test_firmware_200(self):
        status, _, _ = self.get("/dongle/firmware.bin")
        # 200 (file available) or 404 (not yet staged) are both valid
        self.assertIn(status, (200, 404))

    def test_firmware_content_type_when_available(self):
        status, headers, _ = self.get("/dongle/firmware.bin")
        if status == 200:
            ct = headers.get("content-type", "")
            self.assertIn("octet-stream", ct)

    def test_firmware_content_length_matches_body(self):
        status, headers, body = self.get("/dongle/firmware.bin")
        if status == 200:
            cl = headers.get("content-length")
            if cl is not None:
                self.assertEqual(int(cl), len(body))

    def test_firmware_checksum_matches_manifest(self):
        """If both /dongle/latest and /dongle/firmware.bin return 200,
        the SHA-256 of the binary must match the manifest."""
        status_m, _, manifest_body = self.get("/dongle/latest")
        status_f, _, firmware_body = self.get("/dongle/firmware.bin")

        if status_m != 200 or status_f != 200:
            self.skipTest("Firmware or manifest not available")

        manifest = json.loads(manifest_body)
        expected_sha = manifest.get("sha256", "")
        if not expected_sha:
            self.skipTest("No SHA-256 in manifest")

        computed_sha = hashlib.sha256(firmware_body).hexdigest()
        self.assertEqual(
            computed_sha, expected_sha,
            f"Firmware SHA-256 mismatch:\n  manifest: {expected_sha}\n  actual:   {computed_sha}"
        )

    def test_firmware_size_matches_manifest(self):
        """If size is provided in manifest, verify it matches actual binary size."""
        status_m, _, manifest_body = self.get("/dongle/latest")
        status_f, _, firmware_body = self.get("/dongle/firmware.bin")

        if status_m != 200 or status_f != 200:
            self.skipTest("Firmware or manifest not available")

        manifest = json.loads(manifest_body)
        if "size" not in manifest:
            self.skipTest("No size field in manifest")

        self.assertEqual(
            len(firmware_body), manifest["size"],
            f"Firmware size mismatch: manifest={manifest['size']}, "
            f"actual={len(firmware_body)}"
        )


# ---------------------------------------------------------------------------
# Tests — /brain/latest
# ---------------------------------------------------------------------------

class TestBrainLatestEndpoint(OTATestBase):

    def test_brain_latest_200(self):
        status, _, _ = self.get("/brain/latest")
        self.assertEqual(status, 200)

    def test_brain_latest_valid_json(self):
        _, _, body = self.get("/brain/latest")
        try:
            data = json.loads(body)
        except json.JSONDecodeError:
            self.fail(f"/brain/latest not valid JSON: {body!r}")

    def test_brain_latest_has_version(self):
        _, _, body = self.get("/brain/latest")
        data = json.loads(body)
        self.assertIn("version", data)
        self.assertIsInstance(data["version"], str)


# ---------------------------------------------------------------------------
# Tests — 404 for unknown paths
# ---------------------------------------------------------------------------

class TestNotFoundPaths(OTATestBase):

    def test_unknown_path_404(self):
        status, _, _ = self.get("/nonexistent/path")
        self.assertEqual(status, 404)

    def test_typo_in_dongle_path(self):
        status, _, _ = self.get("/dongle/ltest")  # typo of "latest"
        self.assertEqual(status, 404)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)

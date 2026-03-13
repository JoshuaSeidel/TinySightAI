#!/usr/bin/env python3
"""
AADongle OTA Update Server

Runs on the Radxa Cubie A7Z. Serves firmware updates for:
  - Radxa (self-update): system packages, compositor, services
  - T-Dongle-S3: ESP32 firmware binary via HTTP

Endpoints:
  GET  /api/ota/status          — current versions + update availability
  POST /api/ota/check           — check for updates from remote
  POST /api/ota/install/brain   — install Radxa update
  POST /api/ota/install/dongle  — serve T-Dongle firmware for ESP OTA
  GET  /api/ota/dongle/firmware — T-Dongle downloads its firmware here (legacy)

  --- T-Dongle direct-fetch endpoints (used by ota_update.c) ---
  GET  /dongle/latest           — JSON: {version, size, sha256, url}
  GET  /dongle/firmware.bin     — raw ESP32 firmware binary
  GET  /brain/latest            — JSON: {version, size, sha256, url}

Firmware files are stored in /opt/aadongle/firmware/
  t-dongle-s3.bin               — T-Dongle firmware
  brain_update.tar.gz           — Radxa update package

HTTP server on port 8081.
"""

import os
import json
import hashlib
import subprocess
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path

OTA_DIR = Path("/opt/aadongle/ota")
FW_DIR = Path("/opt/aadongle/firmware")   # NEW: firmware storage path
LEGACY_FW_DIR = OTA_DIR / "firmware"      # Legacy path kept for compatibility
VERSION_FILE = OTA_DIR / "versions.json"
UPDATE_URL = os.environ.get("OTA_UPDATE_URL", "")

BRAIN_VERSION = "1.0.0"
DONGLE_VERSION = "1.0.0"

# Ensure directories exist
OTA_DIR.mkdir(parents=True, exist_ok=True)
FW_DIR.mkdir(parents=True, exist_ok=True)
LEGACY_FW_DIR.mkdir(parents=True, exist_ok=True)


def load_versions():
    if VERSION_FILE.exists():
        with open(VERSION_FILE) as f:
            return json.load(f)
    return {"brain": BRAIN_VERSION, "dongle": DONGLE_VERSION}


def save_versions(versions):
    with open(VERSION_FILE, "w") as f:
        json.dump(versions, f, indent=2)


def check_remote_updates():
    """Check remote server for available updates."""
    if not UPDATE_URL:
        return {"available": False, "reason": "No update URL configured"}

    try:
        import urllib.request
        req = urllib.request.Request(f"{UPDATE_URL}/manifest.json")
        with urllib.request.urlopen(req, timeout=10) as resp:
            manifest = json.loads(resp.read())

        versions = load_versions()
        updates = {}

        if manifest.get("brain_version", "") > versions["brain"]:
            updates["brain"] = manifest["brain_version"]
        if manifest.get("dongle_version", "") > versions["dongle"]:
            updates["dongle"] = manifest["dongle_version"]

        return {
            "available": len(updates) > 0,
            "updates": updates,
            "manifest": manifest,
        }
    except Exception as e:
        return {"available": False, "reason": str(e)}


def get_firmware_info(fw_path: Path) -> dict:
    """
    Build the version-info dict for a firmware file.

    Returns a dict suitable for the /dongle/latest or /brain/latest response:
      {version, size, sha256, url}

    Returns None if the firmware file does not exist.
    """
    if not fw_path.exists():
        return None

    with open(fw_path, "rb") as f:
        data = f.read()

    sha256 = hashlib.sha256(data).hexdigest()
    versions = load_versions()

    if fw_path.name.startswith("t-dongle"):
        version = versions.get("dongle", DONGLE_VERSION)
        url = "/dongle/firmware.bin"
    else:
        version = versions.get("brain", BRAIN_VERSION)
        url = "/brain/firmware.tar.gz"

    return {
        "version": version,
        "size":    len(data),
        "sha256":  sha256,
        "url":     url,
    }


def serve_binary_file(handler, fw_path: Path):
    """
    Serve a binary firmware file with the correct Content-Type and
    Content-Length headers.  Sends a 404 JSON error if file missing.
    """
    if not fw_path.exists():
        handler.send_json({"error": "Firmware file not found"}, 404)
        return

    with open(fw_path, "rb") as f:
        data = f.read()

    handler.send_response(200)
    handler.send_header("Content-Type", "application/octet-stream")
    handler.send_header("Content-Length", str(len(data)))
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    handler.wfile.write(data)


def install_brain_update(manifest):
    """Download and install Radxa system update."""
    try:
        import urllib.request
        url = manifest.get("brain_url", "")
        if not url:
            return {"success": False, "error": "No brain update URL"}

        pkg_path = FW_DIR / "brain_update.tar.gz"
        urllib.request.urlretrieve(url, str(pkg_path))

        # Verify checksum
        expected_hash = manifest.get("brain_sha256", "")
        if expected_hash:
            with open(pkg_path, "rb") as f:
                actual_hash = hashlib.sha256(f.read()).hexdigest()
            if actual_hash != expected_hash:
                return {"success": False, "error": "Checksum mismatch"}

        # Extract and run update script
        subprocess.run(["tar", "xzf", str(pkg_path), "-C", str(FW_DIR)], check=True)
        update_script = FW_DIR / "update.sh"
        if update_script.exists():
            subprocess.run(["bash", str(update_script)], check=True)

        versions = load_versions()
        versions["brain"] = manifest["brain_version"]
        save_versions(versions)

        return {"success": True, "version": manifest["brain_version"]}
    except Exception as e:
        return {"success": False, "error": str(e)}


class OTAHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Suppress default logging

    def send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/ota/status":
            versions = load_versions()
            self.send_json({
                "brain_version": versions["brain"],
                "dongle_version": versions["dongle"],
            })

        elif self.path == "/api/ota/dongle/firmware":
            # Legacy endpoint — kept for backward compatibility
            # Prefer /dongle/firmware.bin (used by ota_update.c)
            fw_path = FW_DIR / "t-dongle-s3.bin"
            if not fw_path.exists():
                fw_path = LEGACY_FW_DIR / "t-dongle-s3.bin"
            serve_binary_file(self, fw_path)

        elif self.path == "/dongle/latest":
            # T-Dongle OTA client checks this endpoint for version info
            fw_path = FW_DIR / "t-dongle-s3.bin"
            info = get_firmware_info(fw_path)
            if info:
                self.send_json(info)
            else:
                # Return current version with no download URL when no staged
                # firmware is available — client will see version == current
                # and skip the update.
                versions = load_versions()
                self.send_json({
                    "version": versions.get("dongle", DONGLE_VERSION),
                    "size":    0,
                    "sha256":  "",
                    "url":     "",
                    "available": False,
                })

        elif self.path == "/dongle/firmware.bin":
            # T-Dongle OTA client downloads firmware from this URL
            fw_path = FW_DIR / "t-dongle-s3.bin"
            if not fw_path.exists():
                fw_path = LEGACY_FW_DIR / "t-dongle-s3.bin"
            serve_binary_file(self, fw_path)

        elif self.path == "/brain/latest":
            # Radxa self-update: returns version info for the brain package
            fw_path = FW_DIR / "brain_update.tar.gz"
            info = get_firmware_info(fw_path)
            if info:
                self.send_json(info)
            else:
                versions = load_versions()
                self.send_json({
                    "version": versions.get("brain", BRAIN_VERSION),
                    "size":    0,
                    "sha256":  "",
                    "url":     "",
                    "available": False,
                })

        elif self.path == "/brain/firmware.tar.gz":
            # Radxa brain update package download
            fw_path = FW_DIR / "brain_update.tar.gz"
            serve_binary_file(self, fw_path)

        else:
            self.send_json({"error": "Not found"}, 404)

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else b""

        if self.path == "/api/ota/check":
            result = check_remote_updates()
            self.send_json(result)

        elif self.path == "/api/ota/install/brain":
            result = check_remote_updates()
            if result.get("available") and "brain" in result.get("updates", {}):
                install_result = install_brain_update(result["manifest"])
                self.send_json(install_result)
            else:
                self.send_json({"success": False, "error": "No brain update available"})

        elif self.path == "/api/ota/install/dongle":
            # Download dongle firmware to serve via GET endpoint.
            # Writes to FW_DIR (/opt/aadongle/firmware/) so that
            # /dongle/firmware.bin can serve it.
            result = check_remote_updates()
            if result.get("available") and "dongle" in result.get("updates", {}):
                try:
                    import urllib.request
                    url = result["manifest"].get("dongle_url", "")
                    fw_path = FW_DIR / "t-dongle-s3.bin"
                    urllib.request.urlretrieve(url, str(fw_path))

                    versions = load_versions()
                    versions["dongle"] = result["manifest"]["dongle_version"]
                    save_versions(versions)

                    # Compute and return checksum for verification
                    with open(fw_path, "rb") as f:
                        sha256 = hashlib.sha256(f.read()).hexdigest()

                    self.send_json({
                        "success": True,
                        "message": "Dongle firmware staged",
                        "version": versions["dongle"],
                        "sha256": sha256,
                        "size": fw_path.stat().st_size,
                    })
                except Exception as e:
                    self.send_json({"success": False, "error": str(e)})
            else:
                self.send_json({"success": False, "error": "No dongle update available"})

        else:
            self.send_json({"error": "Not found"}, 404)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()


def main():
    # Initialize version file
    if not VERSION_FILE.exists():
        save_versions({"brain": BRAIN_VERSION, "dongle": DONGLE_VERSION})

    server = HTTPServer(("0.0.0.0", 8081), OTAHandler)
    print(f"OTA server running on port 8081")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("OTA server stopped")
        server.server_close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
AADongle Config & OTA Web Interface

Single management portal on port 80. Provides:
  - Dashboard: service status, system stats, network info
  - Config: WiFi SSID/password, display mode, IR LED settings
  - OTA: firmware upload, version info, remote update check

Config persists to /data/config.json (f2fs partition, survives reboots
even with read-only root + tmpfs overlay).

Talks to:
  - Compositor control: tcp://127.0.0.1:5290
  - OTA server: http://127.0.0.1:8081 (proxied)
"""

import json
import os
import socket
import subprocess
import hashlib
import threading
import tempfile
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SERVER_PORT = 80
COMPOSITOR_HOST = "127.0.0.1"
COMPOSITOR_PORT = 5290
OTA_HOST = "127.0.0.1"
OTA_PORT = 8081

TEMPLATE_DIR = Path(__file__).parent / "templates"
DATA_DIR = Path("/data")
CONFIG_FILE = DATA_DIR / "config.json"
FW_DIR = Path("/opt/aadongle/firmware")

VERSION = "1.0.0"

DEFAULT_CONFIG = {
    "wifi_ssid": "AADongle",
    "wifi_password": "AADongle5GHz!",
    "wifi_channel": 36,
    "wifi_hidden": True,
    "display_mode": "full_aa",
    "ir_led_mode": "auto",
    "ir_led_brightness": 100,
}

# ---------------------------------------------------------------------------
# Config persistence (/data/config.json)
# ---------------------------------------------------------------------------

_config_lock = threading.Lock()


def load_config() -> dict:
    """Load config from /data/config.json, falling back to defaults."""
    with _config_lock:
        config = dict(DEFAULT_CONFIG)
        try:
            if CONFIG_FILE.exists():
                with open(CONFIG_FILE) as f:
                    stored = json.load(f)
                config.update(stored)
        except (json.JSONDecodeError, OSError):
            pass
        return config


def save_config(config: dict) -> bool:
    """Save config to /data/config.json. Returns True on success."""
    with _config_lock:
        try:
            DATA_DIR.mkdir(parents=True, exist_ok=True)
            with open(CONFIG_FILE, "w") as f:
                json.dump(config, f, indent=2)
            return True
        except OSError:
            return False


# ---------------------------------------------------------------------------
# System info helpers
# ---------------------------------------------------------------------------

def get_system_stats() -> dict:
    """Gather system statistics."""
    stats = {}

    # Uptime
    try:
        with open("/proc/uptime") as f:
            uptime_sec = float(f.read().split()[0])
        mins, secs = divmod(int(uptime_sec), 60)
        hours, mins = divmod(mins, 60)
        stats["uptime"] = f"{hours}h {mins}m {secs}s"
    except OSError:
        stats["uptime"] = "unknown"

    # Memory
    try:
        with open("/proc/meminfo") as f:
            lines = f.readlines()
        meminfo = {}
        for line in lines:
            parts = line.split(":")
            if len(parts) == 2:
                key = parts[0].strip()
                val = parts[1].strip().split()[0]
                meminfo[key] = int(val)
        total = meminfo.get("MemTotal", 0) // 1024
        available = meminfo.get("MemAvailable", 0) // 1024
        used = total - available
        stats["ram_total_mb"] = total
        stats["ram_used_mb"] = used
        stats["ram_percent"] = round(used / total * 100, 1) if total else 0
    except OSError:
        stats["ram_total_mb"] = 0
        stats["ram_used_mb"] = 0
        stats["ram_percent"] = 0

    # CPU temperature
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            temp = int(f.read().strip()) / 1000
        stats["cpu_temp_c"] = round(temp, 1)
    except OSError:
        stats["cpu_temp_c"] = 0

    # Disk usage (rootfs)
    try:
        result = subprocess.run(
            ["df", "-h", "/"],
            capture_output=True, text=True, timeout=5
        )
        lines = result.stdout.strip().split("\n")
        if len(lines) >= 2:
            parts = lines[1].split()
            stats["disk_total"] = parts[1] if len(parts) > 1 else "?"
            stats["disk_used"] = parts[2] if len(parts) > 2 else "?"
            stats["disk_percent"] = parts[4] if len(parts) > 4 else "?"
    except (subprocess.TimeoutExpired, OSError):
        stats["disk_total"] = "?"
        stats["disk_used"] = "?"
        stats["disk_percent"] = "?"

    return stats


def get_service_status() -> dict:
    """Check status of all AADongle services."""
    services = [
        "hostapd", "dnsmasq", "bluetooth", "avahi-daemon",
        "aa-bridge", "aa-proxy", "carplay", "baby-monitor",
        "bt-agent", "ir-leds", "power-manager", "config-server",
    ]
    status = {}
    for svc in services:
        try:
            result = subprocess.run(
                ["systemctl", "is-active", svc],
                capture_output=True, text=True, timeout=3
            )
            status[svc] = result.stdout.strip()
        except (subprocess.TimeoutExpired, OSError):
            status[svc] = "unknown"
    return status


def get_network_info() -> dict:
    """Get WiFi AP and connected clients info."""
    info = {}

    # WiFi interface
    try:
        result = subprocess.run(
            ["ip", "addr", "show", "wlan0"],
            capture_output=True, text=True, timeout=3
        )
        info["wlan0"] = result.stdout.strip()
    except (subprocess.TimeoutExpired, OSError):
        info["wlan0"] = "unavailable"

    # Connected clients (hostapd)
    try:
        result = subprocess.run(
            ["hostapd_cli", "all_sta"],
            capture_output=True, text=True, timeout=3
        )
        # Count MAC addresses (lines starting with xx:xx:xx)
        clients = [l for l in result.stdout.split("\n") if len(l) == 17 and l.count(":") == 5]
        info["clients"] = len(clients)
    except (subprocess.TimeoutExpired, OSError, FileNotFoundError):
        info["clients"] = 0

    return info


# ---------------------------------------------------------------------------
# Compositor control
# ---------------------------------------------------------------------------

_compositor_lock = threading.Lock()


def compositor_command(command: str) -> str:
    """Send command to compositor control socket."""
    with _compositor_lock:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(3.0)
            sock.connect((COMPOSITOR_HOST, COMPOSITOR_PORT))
            sock.sendall((command.strip() + "\n").encode())
            chunks = []
            try:
                while True:
                    chunk = sock.recv(4096)
                    if not chunk or b"\n" in chunk:
                        chunks.append(chunk)
                        break
                    chunks.append(chunk)
            except socket.timeout:
                pass
    return b"".join(chunks).decode(errors="replace").strip()


# ---------------------------------------------------------------------------
# Template rendering (simple string substitution)
# ---------------------------------------------------------------------------

def render_template(name: str, **kwargs) -> bytes:
    """Load HTML template and substitute {{key}} placeholders."""
    path = TEMPLATE_DIR / name
    if not path.exists():
        return f"<h1>Template not found: {name}</h1>".encode()

    html = path.read_text()
    for key, value in kwargs.items():
        html = html.replace("{{" + key + "}}", str(value))
    return html.encode()


# ---------------------------------------------------------------------------
# HTTP Handler
# ---------------------------------------------------------------------------

class ConfigHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        pass  # Quiet logs

    def _send_html(self, html: bytes, status: int = 200):
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(html)))
        self.end_headers()
        self.wfile.write(html)

    def _send_json(self, data: dict, status: int = 200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self, location: str):
        self.send_response(303)
        self.send_header("Location", location)
        self.end_headers()

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length > 0 else b""

    # ------------------------------------------------------------------
    # GET routes
    # ------------------------------------------------------------------

    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/" or path == "/dashboard":
            stats = get_system_stats()
            services = get_service_status()
            network = get_network_info()
            config = load_config()

            # Build service status HTML
            svc_html = ""
            for svc, state in services.items():
                color = "#4caf50" if state == "active" else "#f44336"
                dot = f'<span style="color:{color}">&#9679;</span>'
                svc_html += f"<tr><td>{dot} {svc}</td><td>{state}</td></tr>\n"

            html = render_template("index.html",
                version=VERSION,
                uptime=stats["uptime"],
                ram_used=stats["ram_used_mb"],
                ram_total=stats["ram_total_mb"],
                ram_percent=stats["ram_percent"],
                cpu_temp=stats["cpu_temp_c"],
                disk_used=stats["disk_used"],
                disk_total=stats["disk_total"],
                disk_percent=stats["disk_percent"],
                clients=network["clients"],
                wifi_ssid=config["wifi_ssid"],
                display_mode=config["display_mode"],
                services_table=svc_html,
            )
            self._send_html(html)

        elif path == "/config":
            config = load_config()
            html = render_template("config.html",
                wifi_ssid=config["wifi_ssid"],
                wifi_password=config["wifi_password"],
                wifi_channel=config["wifi_channel"],
                wifi_hidden_checked="checked" if config.get("wifi_hidden") else "",
                display_mode=config["display_mode"],
                ir_led_mode=config["ir_led_mode"],
                ir_led_brightness=config["ir_led_brightness"],
                mode_full_aa="selected" if config["display_mode"] == "full_aa" else "",
                mode_full_cp="selected" if config["display_mode"] == "full_carplay" else "",
                mode_full_cam="selected" if config["display_mode"] == "full_camera" else "",
                mode_split_aa="selected" if config["display_mode"] == "split_aa_cam" else "",
                mode_split_cp="selected" if config["display_mode"] == "split_cp_cam" else "",
                ir_auto="selected" if config["ir_led_mode"] == "auto" else "",
                ir_on="selected" if config["ir_led_mode"] == "on" else "",
                ir_off="selected" if config["ir_led_mode"] == "off" else "",
            )
            self._send_html(html)

        elif path == "/ota":
            config = load_config()

            # Check for staged firmware
            dongle_fw = FW_DIR / "t-dongle-s3.bin"
            brain_fw = FW_DIR / "brain_update.tar.gz"
            dongle_staged = dongle_fw.exists()
            brain_staged = brain_fw.exists()

            html = render_template("ota.html",
                version=VERSION,
                dongle_staged="Yes" if dongle_staged else "No",
                brain_staged="Yes" if brain_staged else "No",
            )
            self._send_html(html)

        elif path == "/api/status":
            stats = get_system_stats()
            services = get_service_status()
            config = load_config()
            self._send_json({
                "version": VERSION,
                "config": config,
                "system": stats,
                "services": services,
            })

        elif path == "/api/compositor":
            try:
                status = compositor_command("STATUS")
                self._send_json(json.loads(status))
            except Exception as e:
                self._send_json({"error": str(e)}, 503)

        else:
            self._send_json({"error": "Not found"}, 404)

    # ------------------------------------------------------------------
    # POST routes
    # ------------------------------------------------------------------

    def do_POST(self):
        path = self.path.split("?")[0]
        body = self._read_body()

        if path == "/config":
            # Form submission
            try:
                from urllib.parse import parse_qs
                params = parse_qs(body.decode())

                config = load_config()
                if "wifi_ssid" in params:
                    config["wifi_ssid"] = params["wifi_ssid"][0]
                if "wifi_password" in params:
                    config["wifi_password"] = params["wifi_password"][0]
                if "wifi_channel" in params:
                    config["wifi_channel"] = int(params["wifi_channel"][0])
                config["wifi_hidden"] = "wifi_hidden" in params
                if "display_mode" in params:
                    config["display_mode"] = params["display_mode"][0]
                if "ir_led_mode" in params:
                    config["ir_led_mode"] = params["ir_led_mode"][0]
                if "ir_led_brightness" in params:
                    config["ir_led_brightness"] = int(params["ir_led_brightness"][0])

                save_config(config)
                apply_config(config)
                self._redirect("/config?saved=1")

            except Exception as e:
                self._send_json({"error": str(e)}, 400)

        elif path == "/ota/upload":
            # Firmware file upload (multipart form data)
            content_type = self.headers.get("Content-Type", "")
            if "multipart/form-data" not in content_type:
                self._send_json({"error": "Expected multipart/form-data"}, 400)
                return

            try:
                # Parse boundary
                boundary = content_type.split("boundary=")[1].strip()
                parts = body.split(("--" + boundary).encode())

                for part in parts:
                    if b"filename=" not in part:
                        continue

                    # Extract filename
                    header_end = part.find(b"\r\n\r\n")
                    if header_end == -1:
                        continue
                    header = part[:header_end].decode(errors="replace")
                    file_data = part[header_end + 4:]
                    # Strip trailing \r\n
                    if file_data.endswith(b"\r\n"):
                        file_data = file_data[:-2]

                    # Determine firmware type from filename
                    if "dongle" in header.lower() or "t-dongle" in header.lower():
                        fw_path = FW_DIR / "t-dongle-s3.bin"
                    else:
                        fw_path = FW_DIR / "brain_update.tar.gz"

                    FW_DIR.mkdir(parents=True, exist_ok=True)
                    with open(fw_path, "wb") as f:
                        f.write(file_data)

                    sha256 = hashlib.sha256(file_data).hexdigest()
                    self._redirect(f"/ota?uploaded=1&file={fw_path.name}&sha256={sha256[:12]}")
                    return

                self._send_json({"error": "No file found in upload"}, 400)

            except Exception as e:
                self._send_json({"error": str(e)}, 500)

        elif path == "/ota/check":
            # Proxy to OTA server
            try:
                import http.client
                conn = http.client.HTTPConnection(OTA_HOST, OTA_PORT, timeout=10)
                conn.request("POST", "/api/ota/check")
                resp = conn.getresponse()
                data = json.loads(resp.read())
                conn.close()
                self._send_json(data)
            except Exception as e:
                self._send_json({"error": str(e)}, 503)

        elif path == "/api/reboot":
            self._send_json({"ok": True, "message": "Rebooting in 2 seconds..."})
            threading.Timer(2.0, lambda: subprocess.run(["reboot"])).start()

        else:
            self._send_json({"error": "Not found"}, 404)


# ---------------------------------------------------------------------------
# Apply config changes to running system
# ---------------------------------------------------------------------------

def apply_config(config: dict):
    """Apply config changes to running services (best-effort)."""

    # Update display mode via compositor
    try:
        mode = config.get("display_mode", "full_aa")
        compositor_command(f"MODE {mode}")
    except Exception:
        pass

    # Update IR LED mode
    try:
        ir_mode = config.get("ir_led_mode", "auto")
        compositor_command(f"IR {ir_mode.upper()}")
    except Exception:
        pass

    # Note: WiFi changes require hostapd restart and are NOT applied live
    # to avoid disconnecting the user. They take effect on next boot.


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    # Ensure template dir exists
    if not TEMPLATE_DIR.exists():
        print(f"WARNING: Template directory not found: {TEMPLATE_DIR}")

    # Ensure /data exists (may not if partition not mounted)
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    server = HTTPServer(("0.0.0.0", SERVER_PORT), ConfigHandler)
    print(f"AADongle Config Server running on port {SERVER_PORT}")
    print(f"  Templates: {TEMPLATE_DIR}")
    print(f"  Config:    {CONFIG_FILE}")
    print(f"  Firmware:  {FW_DIR}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Config server stopped")
        server.server_close()


if __name__ == "__main__":
    main()

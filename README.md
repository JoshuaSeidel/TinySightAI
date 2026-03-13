# Baby-Cam

Wireless Android Auto / CarPlay dongle with split-screen baby monitoring for your car. Built from scratch on a Radxa Cubie A7Z (Allwinner A733) and LilyGO T-Dongle-S3 (ESP32-S3).

The car always sees a standard Android Auto device. Behind the scenes, the Radxa intercepts the video stream, composites it with a live baby camera feed, and sends the combined frame back to the car's head unit in real time.

## What It Does

- **Android Auto + Baby Cam split-screen** on your car's existing head unit
- **CarPlay support** via custom MFi chip + AirPlay receiver (no Carlinkit dongle)
- **AI baby monitoring** — detects sleeping, absent, or distress states via 3 TOPS NPU
- **IR night vision** — auto-brightness 850nm LEDs for low-light viewing
- **Phone web UI** at `http://192.168.4.1:8080` — live MJPEG stream, pinch-to-zoom, mode switching, AI alerts
- **Touch controls on the car screen** — overlay buttons for mode cycling and zoom
- **Read-only rootfs** — survives hard power cuts (yanking USB) without SD corruption
- **4-second boot** — parallel systemd services, minimal Debian rootfs (~150MB)

## Architecture

```
                          WiFi 5GHz (hidden AP)
    ┌──────────┐        ┌─────────────────────────────────┐
    │  Phone   │◄──────►│        Radxa Cubie A7Z          │
    │ (AA/CP)  │        │                                 │
    └──────────┘        │  aa-proxy ◄─► compositor ◄─► camera
                        │     │            │                │
                        │     │      ┌─────┴─────┐         │
                        │     │      │G2D+CedarVE│   IR LEDs
                        │     │      │ (HW codec) │         │
                        │     │      └─────┬─────┘         │
                        │     │            │          baby_ai
                        │     ▼            │        (3T NPU)
                        └────────┬─────────┘
                                 │ WiFi
                        ┌────────▼────────┐
                        │  T-Dongle-S3    │
                        │  (ESP32-S3)     │
                        │  USB-A plug     │
                        └────────┬────────┘
                                 │ USB AOA
                        ┌────────▼────────┐
                        │   Car Head Unit │
                        │  (sees AA only) │
                        └─────────────────┘
```

## Hardware

| Part | Role | Notes |
|------|------|-------|
| **Radxa Cubie A7Z 1GB** | Brain | Allwinner A733, WiFi 6, BT 5.4, 31-pin CSI, 3T NPU |
| **LilyGO T-Dongle-S3** | USB bridge | ESP32-S3, USB-A plug, plugs into car |
| **Radxa Camera 4K (2.1mm)** | Baby camera | IMX415 8.29MP, 4K, wide-angle M12 lens |
| **MFi auth chip** | CarPlay auth | Salvaged from MFi accessory, I2C breakout |
| **5mm 850nm IR LEDs** | Night vision | 20mA each, GPIO-driven with 68Ω resistor |
| **High-endurance 32GB microSD** | Storage | Minimal rootfs + persistent /data partition |

## Display Modes

| Mode | Description | Car Screen |
|------|-------------|------------|
| Full AA | Android Auto fullscreen | 1280x720 AA |
| Full CarPlay | CarPlay fullscreen | 1280x720 CP |
| Full Camera | Baby cam fullscreen | 1280x720 camera |
| Split AA+Cam | Side by side | 640x720 AA + 640x720 camera |
| Split CP+Cam | Side by side | 640x720 CP + 640x720 camera |

Switch modes by tapping the [⊞] overlay button on the car screen or via the phone web UI.

## Project Structure

```
baby-cam/
├── t-dongle-s3/                # ESP32-S3 firmware (ESP-IDF)
│   └── main/
│       ├── usb_bridge.c        # TinyUSB AOA device mode
│       ├── wifi_sta.c          # Connect to Radxa WiFi AP
│       ├── tcp_tunnel.c        # USB ↔ TCP bidirectional relay
│       ├── ble_control.c       # BLE GATT for config/status
│       └── led_status.c        # Status LED feedback
│
├── radxa/
│   ├── aa-proxy/               # Rust — AA protocol relay + MITM
│   │   └── src/
│   │       ├── proxy.rs        # TCP/TLS relay, io_uring splice
│   │       ├── mitm.rs         # Video tap + touch remapping
│   │       ├── aap.rs          # AAP frame parsing
│   │       └── tls.rs          # TLS handshake
│   │
│   ├── compositor/             # C — hardware video pipeline
│   │   └── src/
│   │       ├── main.c          # Thread management, lifecycle
│   │       ├── pipeline.c      # CedarVE decode → G2D composite → CedarVE encode
│   │       ├── camera.c        # V4L2 IMX415 capture + zoom
│   │       ├── overlay.c       # On-screen icons + AI indicator
│   │       ├── touch.c         # Car touchscreen coordinate remapping
│   │       ├── control_channel.c # TCP/Unix command dispatch
│   │       ├── mode.c          # Display mode state machine
│   │       ├── ir_led.c        # GPIO IR LED + auto-brightness
│   │       ├── baby_ai.c       # NPU inference (YOLOv8n, 3 TOPS)
│   │       ├── aa_video_input.c  # Receive tapped AA H.264 frames
│   │       ├── aa_video_output.c # Send composited frames back
│   │       ├── aa_emulator.c   # Emulate AA device for CarPlay wrapping
│   │       └── nal_detect.c    # H.264 NAL boundary detection
│   │
│   ├── carplay/                # C — custom CarPlay stack
│   │   ├── mfi/
│   │   │   └── mfi_auth.c     # I2C communication with MFi chip
│   │   ├── iap2/
│   │   │   ├── iap2_session.c # Session management
│   │   │   ├── iap2_auth.c    # Device authentication
│   │   │   ├── iap2_carplay.c # CarPlay session setup
│   │   │   └── iap2_bt_transport.c # Bluetooth RFCOMM transport
│   │   └── airplay/
│   │       ├── airplay_server.c  # mDNS + RTSP server
│   │       ├── airplay_mirror.c  # H.264/H.265 video receive
│   │       ├── airplay_audio.c   # PCM/ALAC/AAC audio
│   │       ├── airplay_pair.c    # Pair-setup (SRP + Ed25519)
│   │       └── airplay_fairplay.c # FairPlay DRM handshake
│   │
│   ├── baby-monitor/           # Phone web UI
│   │   ├── server.py           # HTTP server + MJPEG proxy
│   │   ├── index.html          # Dashboard with live video
│   │   ├── app.js              # Controls, AI alerts, status sync
│   │   └── style.css           # Dark theme, responsive layout
│   │
│   ├── web/                    # Config dashboard (port 80)
│   │   ├── config_server.py    # WiFi, display, OTA settings
│   │   └── templates/          # HTML templates
│   │
│   ├── ota-server/             # Firmware update server
│   │   └── ota_server.py
│   │
│   ├── config/                 # System configuration
│   │   ├── systemd/            # 11 service files + target
│   │   ├── hostapd.conf        # WiFi AP (5GHz, hidden SSID)
│   │   ├── dnsmasq.conf        # DHCP (192.168.4.0/24)
│   │   ├── bluetooth.conf      # BT profiles
│   │   └── overlayfs/          # Read-only root + tmpfs overlay
│   │
│   └── scripts/                # Build & deployment
│       ├── setup.sh            # Master install script (10 phases)
│       ├── build-minimal-rootfs.sh  # Trim OS: 2GB → 150MB
│       ├── make-readonly.sh    # Enable read-only root
│       ├── trim-os.sh          # Remove desktop packages
│       ├── power-manager.sh    # Watchdog + CPU governor
│       └── bt-agent.py         # Bluetooth pairing agent
│
├── enclosure/                  # 3D-printable case (OpenSCAD + STL)
│   ├── camera-unit-body.scad   # Parametric source (65x30mm)
│   ├── camera-unit-body.stl
│   ├── camera-unit-lid.stl     # Camera window + IR LED slots
│   └── t-dongle-sleeve.stl     # USB dongle sleeve
│
├── tests/                      # Python test suite (pytest)
│   ├── test_tcp_tunnel.py
│   ├── test_aap_framing.py
│   ├── test_nal_detection.py
│   ├── test_compositor_control.py
│   ├── test_baby_monitor_api.py
│   ├── test_carplay_video_input.py
│   └── test_ota_server.py
│
└── docs/                       # Design documentation
    ├── split-architecture.md
    ├── hardware-research-update.md
    └── wireless-connectivity.md
```

## Video Pipeline

All video processing is hardware-accelerated on the Allwinner A733:

```
Phone H.264 ──► CedarVE decode ──► YUV frame ─┐
                                                ├──► G2D composite ──► CedarVE encode ──► H.264 out
Camera NV12 ──────────────────────► YUV frame ─┘
                                                         │
                                                    overlay icons
                                                    AI indicator
```

| Stage | Hardware | Latency |
|-------|----------|---------|
| Decode | CedarVE (8K capable) | 3-5ms |
| Composite | G2D 2D engine | 2-3ms |
| Overlay | G2D alpha blend | <1ms |
| Encode | CedarVE (4K@30 capable) | 3-5ms |
| **Total** | | **~10-15ms** |

Output is always 1280x720 H.264 @ 30fps (Android Auto protocol requirement).

## AI Baby Monitoring

Runs on the A733's built-in NPU (3 TOPS @ INT8):

- **Model**: YOLOv8n INT8 (~6MB, ~20ms inference)
- **Inference rate**: ~1 fps in a dedicated thread (doesn't affect video pipeline)
- **Detections**:
  - Baby present / absent
  - Sleeping / awake (pose + stillness)
  - Face covered / distress (triggers alert)
  - Motion level (frame differencing)

Alerts appear as:
- **Color-coded dot** on the car's video overlay (green/blue/orange/red)
- **Dismissable popup** on the phone web UI with severity-specific messaging

## Phone Web UI

Access at `http://192.168.4.1:8080` from any device connected to the Radxa's WiFi AP.

**Controls:**
- Live MJPEG video stream with pinch-to-zoom
- Zoom in/out buttons (digital zoom up to 3.4x)
- IR night vision toggle (off / on / auto)
- Display mode selector (all 5 modes)
- AI baby detection toggle
- AI alert popups (sleeping / absent / distress)
- Firmware version display + OTA update check

**Status bar** shows connection state, current display mode, and zoom level. All UI state syncs from the compositor every 3 seconds.

## Car Touchscreen Controls

Overlay icons are rendered directly on the H.264 video stream:

| Icon | Position | Action |
|------|----------|--------|
| [⊞] | Bottom-right | Cycle display mode |
| [+] | Bottom-right | Zoom in (split/camera modes) |
| [-] | Bottom-right | Zoom out (split/camera modes) |

In split-screen mode, touches on the left half are remapped and forwarded to the phone (AA/CarPlay). Touches on the right half (camera region) are handled locally.

## IR Night Vision

The compositor samples the camera frame's Y-plane brightness every second. When ambient light drops below threshold, IR LEDs turn on automatically. Hysteresis prevents flicker at the boundary (headlights, tunnel transitions).

- **LEDs**: 5mm 850nm, 20mA each, driven from GPIO pin 7 (GPIO3_D4 = 124)
- **Modes**: Off, On, Auto (software-controlled from camera brightness)
- **Threshold**: ON when brightness < 40, OFF when brightness > 55

## Building

### Prerequisites

- Radxa Cubie A7Z running Debian (Radxa official image from radxa-a733 releases)
- ESP-IDF v5.x for T-Dongle firmware
- Rust toolchain for aa-proxy

### On the Radxa (full setup)

```bash
# 1. Flash official Radxa Debian image to SD card, boot

# 2. Clone this repo
git clone https://github.com/JoshuaSeidel/baby-cam.git
cd baby-cam

# 3. Run setup script (builds everything, installs services)
sudo bash radxa/scripts/setup.sh

# 4. Build minimal rootfs (optional — trims 2GB → 150MB)
sudo bash radxa/scripts/build-minimal-rootfs.sh

# 5. Enable read-only root (optional — for production)
sudo bash radxa/scripts/make-readonly.sh

# 6. Reboot
sudo reboot
```

### T-Dongle firmware

```bash
cd t-dongle-s3
idf.py set-target esp32s3
idf.py build
idf.py flash
```

### Tests

```bash
pytest tests/
```

## Network Layout

```
┌─────────────────────────────────────────────┐
│              Radxa WiFi AP                  │
│          SSID: (hidden)                     │
│          IP: 192.168.4.1                    │
│          Band: 5GHz, WPA2                   │
├─────────────────────────────────────────────┤
│                                             │
│  :5277   ← T-Dongle TCP tunnel (AA data)   │
│  :5288   ← Phone AA/TLS connection         │
│  :5290   ← Compositor control channel      │
│  :8080   ← Baby monitor web UI             │
│  :8082   ← µStreamer MJPEG (internal)       │
│  :80     ← Config dashboard                │
│  :8081   ← OTA update server               │
│                                             │
│  /tmp/aa-video.sock       (video tap)       │
│  /tmp/aa-video-out.sock   (video output)    │
│  /tmp/aa-control.sock     (proxy control)   │
│  /tmp/compositor-control.sock               │
│  /tmp/carplay-video.sock  (AirPlay H.264)   │
│                                             │
└─────────────────────────────────────────────┘
```

## Systemd Services

All 11 services run under `aadongle.target` with parallel boot:

| Service | Binary | Port | Description |
|---------|--------|------|-------------|
| `aa-proxy` | Rust | 5277, 5288 | AA protocol relay + MITM |
| `aa-bridge` | C | /tmp/*.sock | Video compositor |
| `carplay` | C | mDNS | iAP2 + AirPlay receiver |
| `baby-monitor` | ustreamer | 8082 | MJPEG camera stream |
| `baby-monitor-api` | Python | 8080 | Web UI + REST API |
| `config-server` | Python | 80 | Config dashboard |
| `ota-server` | Python | 8081 | Firmware updates |
| `bt-agent` | Python | — | Bluetooth pairing |
| `ir-leds` | Shell | — | IR LED GPIO control |
| `power-manager` | Shell | — | Watchdog + CPU governor |
| `hostapd` | System | — | WiFi access point |

## Storage Layout

```
SD Card (32GB):
├── Partition 1: boot     (FAT32, ~64MB)  — kernel, DTB, U-Boot
├── Partition 2: rootfs   (ext4, ~150MB)  — read-only, minimal Debian
│   └── /opt/aadongle/                    — all project binaries + configs
└── Partition 3: data     (f2fs, ~64MB)   — persistent config + OTA state
    └── /data/config.json                 — WiFi SSID, display mode, IR settings
```

## 3D Enclosure

65x30mm footprint (matches Radxa board), ~25mm tall. Stacked design:

1. **Base**: GoPro 2-prong mount (clips to car mount)
2. **Middle**: MFi breakout PCB + Radxa Cubie A7Z
3. **Lid**: Camera window + IR LED slots, snap-fit closure

Print in PETG or ABS. STL files in `enclosure/`.

## License

This project is for personal/educational use.

## Acknowledgments

Built with reference to these open-source projects:

- [aa-proxy-rs](https://github.com/) — Android Auto proxy (Rust)
- [WirelessAndroidAutoDongle](https://github.com/nicholasbalasus/WirelessAndroidAutoDongle) — RPi AA dongle reference
- [uStreamer](https://github.com/pikvm/ustreamer) — MJPEG streamer
- [RPiPlay](https://github.com/FD-/RPiPlay) / shairplay — AirPlay receiver
- [FFmpeg V4L2 M2M](https://trac.ffmpeg.org/wiki/HWAccelIntro) — Allwinner CedarVE hardware codec via V4L2
- [libimobiledevice](https://github.com/libimobiledevice) — Apple protocol implementations

# Implementation Plan — Wireless AA/CarPlay + Split-Screen Baby Monitor

## Final Hardware

| Component | Model | Role |
|---|---|---|
| **Brain** | Radxa Cubie A7Z 1GB (Allwinner A733, WiFi 6, BT 5.4, 3T NPU) | AA/CarPlay proxy, WiFi AP, camera host, video compositor |
| **Camera** | Radxa Camera 4K (IMX415, 2.1mm wide-angle, M12 mount) | Baby monitor with 4K resolution |
| **USB Dongle** | LilyGO T-Dongle-S3 (ESP32-S3, USB-A) | USB bridge at car port |
| **MFi Chip** | Apple MFI341S2164 (QFN-20, I2C breakout) | CarPlay authentication |
| **IR LEDs** | 850nm IR LED module (2x) | Night vision illumination |
| **Storage** | Samsung PRO Endurance 32GB microSD | Radxa OS |

### Why IR LEDs Are Still Needed
The IMX219 **NoIR** removes the infrared filter so the sensor CAN see IR light —
but it doesn't EMIT any. In a dark car at night there's no IR to see. The 850nm
LEDs provide invisible illumination that the NoIR sensor picks up, giving clear
night vision without disturbing the baby.

### Why Debian (Not Alpine)
Radxa Cubie A7Z's vendor kernel (required for A733 hardware video codecs, CSI
camera driver, RGA compositor) only ships for Debian. Alpine uses musl libc
which breaks prebuilt Rockchip MPP libraries. We trim Debian instead: disable
unused services, remove desktop packages, target ~200MB RAM idle.

### MFi Chip Wiring — MFI341S2164 (QFN-20)

| QFN-20 Pin | Signal | Radxa Cubie A7Z | Notes |
|---|---|---|---|
| Pin 13 | SDA (I2C Data) | Phys Pin 3 (I2C3_SDA) | |
| Pin 12 | SCL (I2C Clock) | Phys Pin 5 (I2C3_SCL) | |
| Pin 4 | VCC (3.3V) | Phys Pin 1 (3.3V) | |
| Pin 11 | VSS (Ground) | Phys Pin 6 (GND) | |
| Pin 5 | nRESET | Phys Pin 1 (3.3V) | HIGH = normal operation |
| Pin 2 | MODE1 | 3.3V | HIGH = I2C mode |

**I2C bus**: `/dev/i2c-7` (Radxa uses I2C3 on pins 3/5, not I2C1 like RPi)
**I2C address**: `0x10` (verify with `i2cdetect -y 3`)

All CarPlay software is built and ready — solder the chip to a QFN-20
breakout board, wire 6 connections, and it works.

---

## Architecture

```
 CAR USB-A                              BACKSEAT
 ┌──────────────┐     WiFi 5GHz        ┌───────────────────────────────┐
 │ T-Dongle-S3  │◄─────────────────────►│ Radxa Cubie A7Z                 │
 │ ESP32-S3     │  USB data over TCP    │                               │
 │              │                       │  WiFi AP (hostapd, hidden)    │
 │ USB Device ──┤                       │  BT 5.4 (bluez)              │
 │ WiFi STA     │                       │                               │
 │ BLE Control  │                       │  ┌─────────────────────────┐ │
 └──────┬───────┘                       │  │  Video Compositor       │ │
     Car USB-A                          │  │  (A733 HW codecs)     │ │
                                        │  │                         │ │
  Phone (Android or iPhone)             │  │  ┌─────┐ ┌───────────┐ │ │
      │                                 │  │  │AA/CP│ │ Baby Cam  │ │ │
      │ WiFi 5GHz + BT                  │  │  │Video│ │  IMX219   │ │ │
      └────────────────────────────────►│  │  └──┬──┘ └─────┬─────┘ │ │
                                        │  │     │ COMPOSITE │       │ │
                                        │  │     ▼          ▼       │ │
                                        │  │  ┌──────────────────┐  │ │
                                        │  │  │ H.264 → Car HU   │  │ │
                                        │  │  └──────────────────┘  │ │
                                        │  └─────────────────────────┘ │
                                        │                               │
                                        │  I2C: MFi auth chip (4 wires)│
                                        │  CSI: IMX219 NoIR camera      │
                                        │  GPIO: IR LEDs                │
                                        └───────────────────────────────┘
```

### How It All Connects

Everything goes through the Radxa's single WiFi AP (hidden SSID):
- **T-Dongle** connects as WiFi STA → TCP tunnel for USB data
- **Android phone** connects for AA data (WiFi + BT Classic for discovery)
- **iPhone** connects for CarPlay data (WiFi + BT for discovery + MFi auth)

The car head unit always sees an **Android Auto device** on its USB port
(the T-Dongle). The Radxa controls what video content appears on the car screen.

### Display Modes

| Mode | What Car Shows | Source |
|---|---|---|
| **Full AA** | Standard Android Auto | Phone H.264 via aa-proxy (MITM) |
| **Full CarPlay** | CarPlay UI | iPhone H.264 via custom AirPlay receiver |
| **Full Baby Cam** | Camera feed with zoom | IMX219 H.264 via V4L2 |
| **AA + Baby Cam** | Split: AA left, cam right | Composite both streams |
| **CarPlay + Baby Cam** | Split: CarPlay left, cam right | Composite both streams |

All modes output via AA protocol to the car. In CarPlay/baby-cam-only modes,
the Radxa generates AA-compatible video frames directly (it acts as the "phone"
side of the AA protocol to the car).

### Touch Event Flow

```
Car touchscreen → AA touch → T-Dongle → TCP → Radxa
  → Touch on AA region:      forward to phone (AA protocol)
  → Touch on CarPlay region: forward to iPhone (CarPlay/AirPlay protocol)
  → Touch on baby cam:       handle zoom/pan locally
  → Touch on mode icon:      switch display mode
```

---

## Phase 1: Radxa Cubie A7Z — OS & Base Setup

### 1.1 Flash Debian Image
- Download official Radxa Cubie A7Z Debian CLI image (no desktop)
  from https://github.com/radxa-build/radxa-a733/releases
- Flash to microSD with `dd` or balenaEtcher
- First boot, login via serial console (USB-C OTG port)
- Run `rsetup` for initial configuration
- Trim OS: disable unused services, remove desktop packages

### 1.2 Configure WiFi Access Point (5GHz, Hidden SSID)
```ini
# /etc/hostapd/hostapd.conf
interface=wlan0
driver=nl80211
ssid=AADongle
ignore_broadcast_ssid=1    # Hidden — random phones won't see it
hw_mode=a                  # 5GHz
channel=36
ieee80211n=1
ieee80211ac=1
wpa=2
wpa_passphrase=<generated>
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
max_num_sta=3              # T-Dongle + phone + spare
```

```ini
# /etc/dnsmasq.conf
interface=wlan0
dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,24h
```

T-Dongle and phone both connect to this same AP.

### 1.3 Configure Bluetooth
```ini
# /etc/bluetooth/main.conf
[General]
Name = AADongle
Class = 0x000408
DiscoverableTimeout = 0
AutoEnable=true
```

Two BT profiles registered:
- AA BT profile (UUID `4de17a00-52cb-11e6-bdf4-0800200c9a66`) for Android
- CarPlay BT profile for iPhone discovery + iAP2 auth

### 1.4 Enable IMX219 Camera
```bash
sudo rsetup
# Overlays → Manage overlays → Enable Radxa Camera 4K
# Reboot

sudo apt install v4l-utils
v4l2-ctl --list-devices
v4l2-ctl --device /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12
```

### 1.5 IR LED GPIO Setup
```bash
sudo apt install gpiod
gpioset gpiochip0 <pin>=1  # IR LEDs on via MOSFET
```

### 1.6 Install Allwinner CedarVE + ffmpeg with V4L2 M2M
Critical for hardware video encode/decode (split-screen compositing).
```bash
# CedarVE driver is included in the vendor kernel.
# Build ffmpeg with V4L2 M2M hardware acceleration:
sudo apt install libdrm-dev

git clone https://github.com/FFmpeg/FFmpeg.git
cd FFmpeg
./configure --enable-v4l2-m2m --enable-libdrm --enable-gpl --enable-version3
make -j8 && sudo make install

# Verify:
ffmpeg -decoders | grep v4l2m2m   # h264_v4l2m2m
ffmpeg -encoders | grep v4l2m2m   # h264_v4l2m2m
```

---

## Phase 2: T-Dongle-S3 Firmware (ESP-IDF)

### 2.1 Overview
Transparent USB-to-WiFi bridge:
- **USB**: Appears as Android Open Accessory (AOA) to car head unit
- **WiFi**: Connects to Radxa's hidden AP, tunnels USB data over TCP
- **BLE**: Control channel for status/config/OTA

### 2.2 Project Structure
```
t-dongle-s3/
├── CMakeLists.txt
├── sdkconfig.defaults
└── main/
    ├── main.c              # Init all subsystems
    ├── usb_bridge.c/.h     # TinyUSB AOA device + data handling
    ├── wifi_sta.c/.h       # Connect to Radxa hidden AP
    ├── tcp_tunnel.c/.h     # TCP client, bidirectional USB ↔ TCP
    ├── ble_control.c/.h    # BLE GATT for control/config
    ├── ota_update.c/.h     # OTA firmware update over WiFi
    └── led_status.c/.h     # Status LED feedback
```

### 2.3 USB Device Mode (TinyUSB)
```c
// AOA device descriptors:
// VID: 0x18D1 (Google), PID: 0x2D00 (AOA accessory)
// Device class: Vendor-specific (0xFF)
// Two bulk endpoints (IN + OUT)

// Two startup modes:
// 1. Direct AOA — present as accessory immediately (most head units)
// 2. Legacy switch — start default, respond to AOA handshake, switch

// Data flow:
// tud_vendor_rx_cb() → car sends USB → forward to TCP
// TCP recv → tud_vendor_tx() → send to car
```

### 2.4 WiFi + TCP Tunnel
```c
// 1. Connect to Radxa WiFi AP (SSID/pass via BLE config or hardcoded)
// 2. TCP connect to 192.168.4.1:5277
// 3. Bidirectional forward: USB bulk ↔ TCP socket
// 4. Auto-reconnect on WiFi drop
// Target: < 5ms one-way latency, 10+ Mbps throughput
// TCP_NODELAY on, buffer tuning for low latency
```

### 2.5 BLE Control
```c
// GATT Characteristics:
// - WiFi SSID/Password (read/write)
// - Connection Status (read/notify)
// - Display Mode (write): full_aa, full_cp, full_cam, split_aa, split_cp
// - OTA trigger (write)
```

### 2.6 Build & Flash
```bash
cd t-dongle-s3
idf.py set-target esp32s3
idf.py build
idf.py flash
```

---

## Phase 3: Android Auto Proxy (Radxa)

### 3.1 Modified aa-proxy-rs
aa-proxy-rs normally reads from a local USB gadget fd. We modify it to use
a TCP socket (data from T-Dongle over WiFi).

```
Original:  Phone (WiFi TCP:5277) ↔ aa-proxy-rs ↔ USB gadget fd
Ours:      Phone (WiFi TCP:5277) ↔ aa-proxy-rs ↔ TCP socket (T-Dongle)
```

The io_uring splice operations work on TCP sockets the same way.

### 3.2 MITM Mode for Video Interception
To composite baby cam into the AA video stream:

```
Phone ←TLS→ aa-proxy-rs (MITM) ←TLS→ Car HU (via T-Dongle)
                  │
                  ├── Channel 3 (Video): intercept H.264
                  │     → Decode (h264_v4l2m2m hardware)
                  │     → Composite with baby cam (RGA)
                  │     → Re-encode (h264_v4l2m2m hardware)
                  │     → Forward to car
                  │
                  ├── Channel 1 (Touch): remap coordinates
                  │     → Split-screen mapping
                  │
                  └── All other channels: pass through
```

### 3.3 AA Protocol Video Format
```
AAP Frame (6-byte header):
  byte 0:     channel_id (3 = video)
  byte 1:     flags
  bytes 2-3:  payload length (big-endian)
  bytes 4-7:  total size (fragmented only)

Video payload:
  bytes 0-1:  message type
  bytes 2-9:  timestamp (nanoseconds)
  bytes 10+:  H.264 Annex B NAL units

Frames > 16KB are fragmented — must reassemble before decode.
```

### 3.4 Hardware Video Pipeline (A733)
```
AA/CarPlay H.264 → RKVDEC2 (HW decode) → YUV frame ──┐
                                                        │
IMX219 camera → V4L2 capture → YUV frame ──────────────┤
                                                        ▼
                                                   RGA Compositor
                                                   (hardware 2D)
                                                        │
                                                        ▼
                                                  Composited YUV
                                                        │
                                                        ▼
                                              Hantro H1 (HW encode)
                                                        │
                                                        ▼
                                              H.264 → AAP frames → car

Latency at 720p 30fps (33ms budget):
  HW decode:     ~5-8ms
  RGA composite: ~2-3ms
  HW encode:     ~5-8ms
  Framing:       ~1ms
  Total:         ~15-20ms ✓
```

---

## Phase 4: CarPlay Stack (Custom, No Carlinkit)

This is our own CarPlay implementation. All software runs on the Radxa.
The only Apple-specific hardware is the MFi auth chip on I2C.

### 4.1 Architecture

```
iPhone                              Radxa Cubie A7Z
  │                                    │
  │◄──── BT discovery ────────────────►│ bluez: CarPlay BT service
  │                                    │
  │◄──── iAP2 over BT ───────────────►│ iap2d: iAP2 protocol daemon
  │      (MFi challenge/response)      │   └── MFi chip on I2C signs challenge
  │                                    │
  │◄──── WiFi connect ───────────────►│ hostapd: iPhone joins our AP
  │                                    │
  │◄──── AirPlay session ────────────►│ airplay-receiver: custom service
  │      H.264 video + audio OUT       │   ├── receives H.264 video
  │      touch/Siri/controls IN        │   ├── receives audio (PCM/AAC)
  │                                    │   ├── sends touch events
  │                                    │   └── sends Siri audio
  │                                    │
  │                                    │ compositor: same pipeline as AA
  │                                    │   └── H.264 → composite → AA → car
```

### 4.2 MFi I2C Driver
```c
// mfi_auth.c — talks to Apple MFi coprocessor over I2C
// Chip address: typically 0x10 or 0x11

#include <linux/i2c-dev.h>

// MFi auth flow:
// 1. iPhone sends 128-byte challenge via iAP2
// 2. We write challenge to MFi chip register 0x00
// 3. Write 0x01 to control register to trigger signing
// 4. Poll status register until signing complete (~50ms)
// 5. Read 128-byte signed response from register 0x10
// 6. Return signed response to iPhone via iAP2
// 7. iPhone verifies Apple CA signature chain → success

// The chip handles all RSA crypto internally.
// We never see the private key — it's fused into silicon.

typedef struct {
    int i2c_fd;
    uint8_t addr;
} mfi_device_t;

int mfi_open(mfi_device_t *dev, const char *i2c_bus, uint8_t addr);
int mfi_sign_challenge(mfi_device_t *dev, const uint8_t *challenge,
                       size_t challenge_len, uint8_t *signature,
                       size_t *sig_len);
int mfi_get_certificate(mfi_device_t *dev, uint8_t *cert, size_t *cert_len);
void mfi_close(mfi_device_t *dev);
```

### 4.3 iAP2 Protocol Daemon
iAP2 (iPod Accessory Protocol 2) handles device identification, auth, and
session setup between the iPhone and our device.

```
radxa/carplay/
├── iap2/
│   ├── iap2_link.c/.h        # Link layer (framing, checksum, sequence)
│   ├── iap2_session.c/.h     # Session layer (control, data sessions)
│   ├── iap2_auth.c/.h        # MFi auth message handling (calls mfi_auth)
│   ├── iap2_carplay.c/.h     # CarPlay-specific session messages
│   └── iap2_bt_transport.c/.h # Bluetooth RFCOMM transport
```

Key iAP2 messages:
- `IdentificationInformation` — we declare supported features (CarPlay)
- `StartExternalAccessoryProtocolSession` — starts CarPlay data session
- `RequestAuthenticationCertificateSerial` — iPhone asks for our MFi cert
- `AuthenticationResponse` — we return MFi-signed challenge

Reference: wiomoc.de iAP2 research + libimobiledevice partial implementations.

### 4.4 AirPlay Receiver
Once iAP2 auth succeeds, CarPlay uses AirPlay 2 for media streaming.
We build on existing open-source AirPlay receiver code (RPiPlay/shairplay lineage):

```
radxa/carplay/
├── airplay/
│   ├── airplay_server.c/.h    # mDNS advertisement + RTSP server
│   ├── airplay_mirror.c/.h    # Screen mirroring (H.264 receive)
│   ├── airplay_audio.c/.h     # Audio receive (PCM/ALAC/AAC)
│   ├── airplay_input.c/.h     # Touch/HID event injection to iPhone
│   ├── airplay_crypto.c/.h    # Pair-setup, pair-verify (SRP + Ed25519)
│   └── fairplay.c/.h          # FairPlay handshake (required by AirPlay)
```

The AirPlay receiver:
1. Advertises via mDNS (`_airplay._tcp`)
2. iPhone connects RTSP
3. Pair-verify handshake (Ed25519, cached after first pairing)
4. Receives H.264 video stream (same format as what Carlinkit outputs)
5. Receives audio
6. Sends touch events back to iPhone

### 4.5 CarPlay-Specific Extensions
CarPlay adds control channels on top of AirPlay:
- **Navigation**: Turn-by-turn data, map tiles
- **Phone**: Call state, contacts
- **Media**: Now playing, queue management
- **Siri**: Audio capture from car mic → iPhone
- **Vehicle**: Night mode, speed (for UI adaptation)

These are protobuf-encoded messages on a separate data channel.
We implement the minimum required set for a working CarPlay display.

### 4.6 CarPlay → AA Protocol Wrapping
When in CarPlay mode, the Radxa outputs to the car as AA protocol:

```
CarPlay H.264 from iPhone
  → (optional) composite with baby cam
  → wrap as AA video frames (channel 3, with timestamps)
  → send via TCP to T-Dongle
  → T-Dongle forwards over USB to car

Car touch events (AA protocol)
  → Radxa receives via T-Dongle TCP
  → remap touch coordinates
  → convert to AirPlay HID touch events
  → send to iPhone via AirPlay input channel
```

The car always thinks it's running Android Auto.

### 4.7 FairPlay Note
AirPlay 2 requires a FairPlay handshake. This is separate from MFi auth.
Open-source implementations handle this via a small binary blob approach
(same as RPiPlay). This is the pragmatic path — implementing FairPlay
from scratch is not feasible (Apple's DRM).

---

## Phase 5: Video Compositor Service

### 5.1 Unified Compositor
A single service handles all display modes. It takes input from multiple
sources and outputs a single H.264 stream wrapped in AA protocol.

```
radxa/compositor/
├── src/
│   ├── main.c                 # Service entry, mode management
│   ├── pipeline.c/.h          # MPP decode → RGA composite → encode
│   ├── aa_protocol.c/.h       # AAP frame parsing and generation
│   ├── camera.c/.h            # V4L2 IMX219 capture + zoom/crop
│   ├── overlay.c/.h           # Mode switch icons, zoom buttons
│   ├── touch.c/.h             # Touch coordinate remapping
│   └── mode.c/.h              # Display mode state machine
├── Makefile
└── compositor.service          # systemd unit
```

### 5.2 Display Mode Layouts

```
Full AA/CarPlay:                Split (AA/CP + Baby Cam):
┌──────────────────────────┐    ┌─────────────┬──────────────┐
│                          │    │             │              │
│     AA or CarPlay        │    │  AA or CP   │  Baby Cam    │
│     (1280x720)           │    │  (640x720)  │  (640x720)   │
│                          │    │             │              │
│                   [⊞]    │    │             │  [+][-] [⊞]  │
└──────────────────────────┘    └─────────────┴──────────────┘

Full Baby Cam:
┌──────────────────────────┐
│                          │
│       Baby Camera        │
│       (1280x720)         │
│                          │
│       [+] [-] [⊞]       │
└──────────────────────────┘

[⊞] = mode cycle    [+][-] = zoom in/out
```

### 5.3 Touch Handling
- **Pinch-to-zoom**: If car touchscreen supports multi-touch via AA
- **Zoom icons**: [+] and [-] for single-touch screens
- **Mode switch**: [⊞] cycles through modes
- **Long-press [⊞]**: Toggle between AA and CarPlay source
- Icons are composited into the H.264 stream via RGA overlay
- Touch coordinates on icons intercepted by Radxa (not forwarded)

---

## Phase 6: Baby Monitor Camera Service

### 6.1 V4L2 Camera Capture
Two consumers of the IMX219:
1. **Compositor** — reads frames for split-screen to car display
2. **µStreamer** — MJPEG for phone web UI (secondary view)

```bash
sudo apt install build-essential libevent-dev libjpeg62-turbo-dev
git clone https://github.com/pikvm/ustreamer
cd ustreamer && make
```

### 6.2 Digital Zoom
8MP IMX219 → 3.4x digital zoom at 720p:
- Zoom 1x: Full 3280x2464 → scale to 720p (wide view)
- Zoom 2x: Center crop 1640x1232 → scale to 720p
- Zoom 3.4x: Center crop 960x720 → direct output (max zoom)

Via V4L2 crop + RGA scale (hardware accelerated).

### 6.3 Phone Web UI
Served at `http://192.168.4.1:8080`:
```
radxa/baby-monitor/
├── index.html    # MJPEG viewer + pinch-to-zoom
├── style.css     # Dark-mode UI
└── app.js        # Zoom, IR toggle, mode switch, OTA check
```

---

## Phase 7: OTA Update System

### 7.1 Architecture
```
Update source (GitHub Releases or self-hosted)
        │
        ▼
┌─────────────────┐
│  Radxa OTA      │  :8081/api/ota/
│  Server         │
├─────────────────┤
│ /brain/version  │  Current Radxa version
│ /brain/update   │  Trigger self-update
│ /dongle/version │  T-Dongle firmware version
│ /dongle/firmware│  Serve T-Dongle binary
│ /status         │  Update progress
└────────┬────────┘
    ┌────┴────┐
    ▼         ▼
T-Dongle    Phone/Car UI
(ESP OTA)   (web trigger)
```

### 7.2 Radxa Self-Update
- Daemon checks configured URL for new versions
- Downloads update package, verifies checksum
- Applies update, restarts services
- Rollback on failure

### 7.3 T-Dongle OTA
- Checks Radxa HTTP server on boot
- ESP-IDF dual OTA partitions for safe rollback

### 7.4 Update UI
- Phone: `http://192.168.4.1:8081/update`
- Car display: settings icon in mode overlay

---

## Phase 8: 3D Printed Case

### 8.1 Dimensions
65x30mm footprint (matches Radxa), stacked vertically.

```
Side cross-section:          Front view:
┌──────────────────┐         ┌──────────────────┐
│ Camera + IR LEDs │ ~8mm    │ [IR] [CAM] [IR]  │
├──────────────────┤         │                  │
│ Radxa Cubie A7Z   │ ~6mm    │                  │
├──────────────────┤         │                  │
│ MFi breakout PCB│ ~3mm    │                  │
├──────────────────┤         │                  │
│ GoPro mount     │ ~8mm    └──────────────────┘
└──────────────────┘              65mm wide
  Total: ~25mm tall
```

Stack (bottom to top):
1. GoPro 2-prong mount fingers (3mm wide, 3mm gap, 5mm hole)
2. MFi breakout board (tiny, ~15x10mm)
3. Radxa Cubie A7Z (65x30mm)
4. IMX219 camera (24x25mm center) + IR LEDs (flanking)

Access ports (sides): USB-C power, microSD slot.
CSI ribbon routes internally from Radxa to camera.

### 8.2 Files
```
enclosure/
├── camera-unit-body.scad    # OpenSCAD parametric source
├── camera-unit-body.stl
├── camera-unit-lid.stl      # Snap-fit top (camera window)
└── t-dongle-sleeve.stl      # Optional T-Dongle sleeve
```

---

## Phase 9: Integration & Testing

### 9.1 Bench Test
1. Radxa: WiFi AP, BT, camera, µStreamer, hardware codecs
2. T-Dongle: WiFi connect, TCP tunnel, USB AOA
3. AA passthrough: phone → Radxa → T-Dongle → USB host (PC simulating car)
4. Baby cam compositing: hardware decode → RGA composite → encode
5. CarPlay stack: BT discovery, iAP2 (needs MFi chip to complete auth)

### 9.2 Car Test — Android Auto
1. Plug T-Dongle into car USB
2. Power Radxa via 12V → USB-C
3. Phone pairs via BT, AA appears on car screen
4. Test split-screen, mode switching, zoom
5. Test night vision with IR LEDs

### 9.3 Car Test — CarPlay (after MFi chip soldered)
1. Same physical setup
2. iPhone discovers via BT, iAP2 auth completes
3. AirPlay session starts, CarPlay on car screen
4. Test split-screen, touch translation

---

## Development Order

| Step | What | Parallel? | Target |
|---|---|---|---|
| 1 | Radxa OS, WiFi AP (hidden), BT, camera, MPP/ffmpeg | — | Week 1 |
| 2 | T-Dongle firmware: USB AOA + WiFi + TCP + BLE | Yes, with 1 | Week 1-2 |
| 3 | aa-proxy-rs: TCP socket mod, basic AA passthrough | After 1 | Week 2 |
| 4 | T-Dongle ↔ Radxa integration, AA working on car | After 2+3 | Week 2-3 |
| 5 | Baby cam: µStreamer + web UI + V4L2 pipeline | After 1 | Week 3 |
| 6 | MITM + hardware video compositor (split-screen) | After 4+5 | Week 3-5 |
| 7 | CarPlay: MFi driver, iAP2 daemon, AirPlay receiver | After 1 | Week 3-6 |
| 8 | CarPlay → AA wrapping + compositor integration | After 6+7 | Week 6-7 |
| 9 | Display mode UI overlay + touch remapping | After 6 | Week 7 |
| 10 | OTA update system | After 4 | Week 7-8 |
| 11 | 3D case design + print | Any time | Week 8 |
| 12 | Polish: auto-start, IR auto, power mgmt, testing | Last | Week 8-9 |

Steps 1+2 and 3+7 run in parallel. CarPlay software (step 7) can be built
and tested up to the MFi auth point without the physical chip — everything
else (iAP2 framing, AirPlay receiver, compositor integration) works without it.

---

## Project Directory Structure

```
espwirelesscar/
├── IMPLEMENTATION_PLAN.md
├── t-dongle-s3/                    # ESP-IDF project
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│       ├── main.c
│       ├── usb_bridge.c/.h
│       ├── wifi_sta.c/.h
│       ├── tcp_tunnel.c/.h
│       ├── ble_control.c/.h
│       ├── ota_update.c/.h
│       └── led_status.c/.h
├── radxa/
│   ├── aa-proxy/                   # Modified aa-proxy-rs fork
│   ├── carplay/                    # Custom CarPlay stack
│   │   ├── mfi/                    # MFi I2C auth driver
│   │   │   ├── mfi_auth.c/.h
│   │   │   └── mfi_i2c.c/.h
│   │   ├── iap2/                   # iAP2 protocol
│   │   │   ├── iap2_link.c/.h
│   │   │   ├── iap2_session.c/.h
│   │   │   ├── iap2_auth.c/.h
│   │   │   └── iap2_bt_transport.c/.h
│   │   ├── airplay/                # AirPlay receiver
│   │   │   ├── airplay_server.c/.h
│   │   │   ├── airplay_mirror.c/.h
│   │   │   ├── airplay_audio.c/.h
│   │   │   ├── airplay_input.c/.h
│   │   │   └── airplay_crypto.c/.h
│   │   └── Makefile
│   ├── compositor/                 # Video compositor
│   │   ├── src/
│   │   │   ├── main.c
│   │   │   ├── pipeline.c/.h
│   │   │   ├── aa_protocol.c/.h
│   │   │   ├── camera.c/.h
│   │   │   ├── overlay.c/.h
│   │   │   ├── touch.c/.h
│   │   │   └── mode.c/.h
│   │   └── Makefile
│   ├── baby-monitor/               # Phone web UI
│   │   ├── index.html
│   │   ├── style.css
│   │   └── app.js
│   ├── ota-server/                 # OTA HTTP server
│   ├── config/                     # System configs
│   │   ├── hostapd.conf
│   │   ├── dnsmasq.conf
│   │   └── systemd/
│   └── scripts/
│       ├── setup.sh
│       ├── trim-os.sh
│       └── install-mpp.sh
├── enclosure/                      # 3D printable cases
│   ├── camera-unit-body.scad
│   ├── camera-unit-body.stl
│   ├── camera-unit-lid.stl
│   └── t-dongle-sleeve.stl
└── docs/
```

---

## Key Risks & Mitigations

| Risk | Mitigation |
|---|---|
| aa-proxy-rs MITM certs hard to obtain | Extract galroot_cert from AA app; fallback to phone-based split |
| A733 CedarVE encode issues | Use ffmpeg V4L2 M2M h264_v4l2m2m; fallback to software encode |
| MFi chip I2C register map varies by chip version | Test with multiple salvaged chips; community I2C dumps exist |
| iAP2 protocol gaps in documentation | Reference libimobiledevice, wiomoc.de research, packet captures |
| FairPlay handshake changes in new iOS | Pin supported iOS versions; RPiPlay community tracks changes |
| T-Dongle USB tunnel latency > 10ms | TCP_NODELAY, buffer tuning; fallback to direct USB cable |
| 1GB RAM tight | Trim OS to ~200MB; compositor uses zero-copy DRM buffers |
| Car HU rejects modified AA frames | Test unmodified passthrough first; match exact timing |

---

## Fallback: Direct USB (No T-Dongle)

If the T-Dongle tunnel proves unreliable, Radxa connects directly via
USB-C OTG cable to car:
```bash
sudo modprobe libcomposite
# Configure AOA gadget via /sys/kernel/config/usb_gadget/
```
Requires USB cable from backseat to dashboard. All other features unchanged.

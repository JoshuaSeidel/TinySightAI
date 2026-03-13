# AADongle — First Boot Checklist

When all hardware arrives, follow this checklist to go from bare boards to a
working system.

---

## Phase 0: Pre-assembly (on your dev machine)

- [ ] Download Radxa Cubie A7Z official Debian image
      https://github.com/radxa-build/radxa-a733/releases
- [ ] Flash image to high-endurance microSD with `balenaEtcher` or `dd`
- [ ] Download ESP-IDF v5.x toolchain (for T-Dongle firmware)
      https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/

---

## Phase 1: Radxa Cubie A7Z initial setup

### 1.1 First boot (stock Debian)
- [ ] Insert microSD, connect USB-C power + ethernet (or HDMI + keyboard)
- [ ] Boot and log in (default: `radxa` / `radxa`)
- [ ] Set hostname: `sudo hostnamectl set-hostname aadongle`
- [ ] Connect to WiFi or ethernet for internet access
- [ ] `sudo apt update && sudo apt upgrade -y`

### 1.2 Clone and build
```bash
git clone <your-repo-url> ~/espwirelesscar
cd ~/espwirelesscar
sudo bash radxa/scripts/setup.sh
```
This builds ALL components (~20-30 min on first run):
- [2/10] System packages (hostapd, bluez, v4l-utils, MPP, RGA, etc.)
- [3/10] Allwinner CedarVE (hardware codecs)
- [4/10] aa-proxy (Rust — installs rustup if needed)
- [5/10] Compositor (C — links against MPP/RGA)
- [6/10] CarPlay stack (C — links against openssl, avahi, bluez, ffmpeg)
- [7/10] T-Dongle cross-compile (skipped on Radxa — do this on dev machine)
- [8/10] WiFi AP (hostapd + dnsmasq)
- [9/10] Systemd services (11 units + aadongle.target)
- [10/10] Web interface + OTA server

### 1.3 Verify services start
```bash
sudo systemctl start aadongle.target
systemctl status aadongle.target
systemctl list-units 'aa-*' 'carplay*' 'baby-*' 'compositor*' 'config-*' 'bt-*' 'ir-*' 'power-*'
```
Expected: all services active (some may show "waiting for device" — that's OK
before camera/MFi chip are wired).

### 1.4 Test WiFi AP
- [ ] From phone/laptop, scan for SSID `AADongle` (or whatever you configured)
- [ ] Connect, verify DHCP lease (should get 192.168.4.x)
- [ ] Browse to http://192.168.4.1 — dashboard should load

### 1.5 Build minimal rootfs (optional but recommended)
```bash
sudo bash radxa/scripts/build-minimal-rootfs.sh
```
Trims OS from ~2GB to ~150MB. Creates a backup first.
After reboot, verify:
```bash
df -h /                    # Should be <200MB
free -h                    # Idle RAM <150MB
systemd-analyze            # Boot time <5s
```

### 1.6 Enable read-only root
```bash
sudo bash radxa/scripts/make-readonly.sh
```
After reboot, root is read-only with tmpfs overlay. Hard power cuts are safe.
To make persistent changes later:
```bash
sudo mount -o remount,rw /overlay/lower
# ... make changes ...
sudo mount -o remount,ro /overlay/lower
```

---

## Phase 2: Camera (Radxa Camera 4K)

### 2.1 Connect
- [ ] Power off Radxa
- [ ] Connect Radxa Camera 4K to 31-pin CSI connector (gold contacts face PCB)
- [ ] Power on

### 2.2 Verify
```bash
# Check device node exists
ls /dev/video0

# Test capture
v4l2-ctl --device /dev/video0 --stream-mmap --stream-count=10

# Check resolution
v4l2-ctl --device /dev/video0 --list-formats-ext
```

### 2.3 Test baby monitor
```bash
# Start baby monitor service
sudo systemctl start baby-monitor.service

# From phone/laptop on WiFi AP:
# Browse to http://192.168.4.1:8080
# Should see live MJPEG camera feed
```

---

## Phase 3: IR LEDs (850nm)

### 3.1 Wire
- [ ] IR LED module VCC → 3.3V (or 5V depending on module)
- [ ] IR LED module GND → GND
- [ ] IR LED module signal/enable → GPIO pin (update pin number in ir-led-control.sh)
- [ ] If using MOSFET: GPIO → gate, LED VCC → drain, GND → source

### 3.2 Configure GPIO pin
Edit `/opt/aadongle/scripts/ir-led-control.sh` — set `IR_GPIO` to your pin number.

### 3.3 Test
```bash
# Manual on/off
sudo bash /opt/aadongle/scripts/ir-led-control.sh on
sudo bash /opt/aadongle/scripts/ir-led-control.sh off

# Via web UI
# http://192.168.4.1/config → IR LEDs section
```

### 3.4 Night vision test
- [ ] In dark room, set IR to "on" via web UI
- [ ] Check baby monitor feed — should see IR-illuminated grayscale image
- [ ] Test "auto" mode if module has photoresistor

---

## Phase 4: T-Dongle-S3 firmware

### 4.1 Build (on dev machine with ESP-IDF)
```bash
cd t-dongle-s3
idf.py set-target esp32s3
idf.py build
```

### 4.2 Flash
- [ ] Connect T-Dongle to dev machine via USB
- [ ] Hold BOOT button, press RESET, release BOOT
```bash
idf.py -p /dev/ttyACM0 flash    # adjust port as needed
idf.py -p /dev/ttyACM0 monitor  # watch boot log
```

### 4.3 Verify
Expected boot log:
```
WiFi STA connecting to AADongle...
WiFi connected, IP: 192.168.4.x
TCP tunnel connecting to 192.168.4.1:5277...
TCP tunnel connected
USB bridge initialized (AOA mode)
```

### 4.4 Test with car
- [ ] Plug T-Dongle into car USB-A port
- [ ] Car should detect it as an Android Auto accessory
- [ ] T-Dongle LED should go solid (connected)
- [ ] If no phone connected: standalone mode — compositor video → car display

---

## Phase 5: Android Auto (phone connected)

### 5.1 Test AA relay
- [ ] T-Dongle plugged into car
- [ ] Android phone connected to AADongle WiFi
- [ ] Phone should auto-detect AA wireless → connect
- [ ] Car display shows Android Auto

### 5.2 Test split-screen (AA + Camera)
- [ ] From web UI or control channel: set mode to "split_aa_cam"
- [ ] Car display should show AA on top half, baby cam on bottom
- [ ] Touch AA region → works normally
- [ ] Touch camera region → no response (correct)

### 5.3 Test mode switching
- [ ] Switch between all modes via web UI:
  - Full Android Auto
  - Full Camera
  - Split: AA + Camera
  - (CarPlay modes require Phase 6)

---

## Phase 6: MFi chip + CarPlay

### 6.1 Solder MFi chip
- [ ] Solder MFI341S2164 (QFN-20) to breakout board
- [ ] Wire 6 connections:

| MFi Pin   | Connect To              |
|-----------|-------------------------|
| SDA       | Radxa pin 3 (I2C3_SDA) |
| SCL       | Radxa pin 5 (I2C3_SCL) |
| VCC       | Radxa pin 1 (3.3V)     |
| GND       | Radxa pin 6 (GND)      |
| nRESET    | 3.3V (pull high)        |
| MODE1     | 3.3V (I2C slave mode)   |

### 6.2 Verify I2C
```bash
# Scan I2C bus 3
sudo i2cdetect -y 3
# Should show device at address 0x10

# Read MFi certificate (first bytes)
sudo i2cget -y 3 0x10 0x00
```

### 6.3 Test CarPlay
- [ ] iPhone connected to AADongle WiFi + Bluetooth
- [ ] iPhone should detect CarPlay wireless → connect
- [ ] Car display shows CarPlay UI

### 6.4 Test CarPlay + Camera split
- [ ] Set mode to "split_cp_cam"
- [ ] Car shows CarPlay on top, baby cam on bottom

---

## Phase 7: 3D Enclosure

### 7.1 Print
- [ ] Print `enclosure/camera-unit-body.stl` (PETG or ABS recommended)
- [ ] Print `enclosure/camera-unit-lid.stl`

### 7.2 Assemble (bottom to top)
1. GoPro 2-prong mount (clips into body base)
2. MFi breakout board (sits in middle slot)
3. Radxa Cubie A7Z (press-fit, USB-C accessible from side)
4. Camera module + IR LEDs (mount in lid, ribbon cable threads through)
5. Snap lid onto body

### 7.3 Mount
- [ ] Attach to car headrest/visor/mirror via GoPro mount
- [ ] Aim camera at baby seat
- [ ] Route USB-C power cable

---

## Phase 8: Final validation

### 8.1 Boot time
```bash
systemd-analyze                    # Target: <5s
systemd-analyze blame | head -20   # Identify slow services
```

### 8.2 Hard power cut test
- [ ] Yank USB power 10 times
- [ ] Each time: verify clean boot, all services come up
- [ ] No filesystem corruption (read-only root protects this)

### 8.3 Full integration
- [ ] T-Dongle in car USB
- [ ] Android phone → AA works
- [ ] iPhone → CarPlay works (with MFi chip)
- [ ] Baby cam visible in split modes
- [ ] Night vision works in dark
- [ ] Web UI at 192.168.4.1 — all controls functional
- [ ] OTA page — can upload firmware

### 8.4 Heat test
```bash
# Monitor CPU temp during heavy use (AA + camera + compositing)
watch -n1 cat /sys/class/thermal/thermal_zone0/temp
# Should stay below 80°C — if higher, consider thermal pad in enclosure
```

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| No WiFi AP | `journalctl -u hostapd` — check channel/driver |
| T-Dongle won't connect | `journalctl -u aa-proxy` — check port 5277 |
| No AA on phone | `journalctl -u aa-proxy` — check TLS handshake |
| No video on car | `journalctl -u compositor` — check pipeline init |
| CarPlay not detected | `journalctl -u carplay` — check MFi I2C |
| Camera black | `v4l2-ctl --list-devices` — check CSI ribbon |
| Boot slow | `systemd-analyze blame` — identify bottleneck |
| Filesystem full | `mount -o remount,rw /overlay/lower` to fix |

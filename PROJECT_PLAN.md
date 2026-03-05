# Wireless CarPlay/Android Auto Dongle + Baby Monitor Camera

## Project Overview

Build a device that:
1. Plugs into a car's USB-A data port
2. Wirelessly connects to an iPhone (CarPlay) or Android phone (Android Auto)
3. Wirelessly connects to an in-car camera for split-screen baby monitoring

---

## Hard Truths: Why ESP32/Arduino Alone Won't Work

Before getting into the plan, here's what the research reveals:

| Requirement | ESP32-S3 (best ESP32 variant) | What's Actually Needed |
|---|---|---|
| USB to car | USB Full-Speed 12 Mbps (marginal) | USB Full-Speed minimum, High-Speed preferred |
| Run CarPlay/AA protocol stack | No Linux, 512 KB RAM | Linux OS, 512 MB+ RAM |
| H.264 video decode | Software-only, ~15 fps at 480p | Hardware decode at 720p+ 30fps |
| Wi-Fi 5 GHz (CarPlay requirement) | 2.4 GHz only | 802.11ac dual-band |
| Bluetooth Classic (CarPlay discovery) | BLE + Classic | BT Classic + BLE |
| Apple MFi authentication | Impossible without MFi chip | Licensed MFi auth coprocessor |

**Bottom line**: No ESP32 or Arduino can run the CarPlay or Android Auto protocol stack. These protocols require a Linux OS, significant RAM, and (for CarPlay) Apple's proprietary authentication hardware.

### What ESP32 CAN Do
- Run the baby monitor camera (ESP32-CAM) — this is a great fit
- Potentially serve as a Bluetooth/Wi-Fi coprocessor alongside a main board

---

## Recommended Architecture

### Two-Module Design

```
┌─────────────────────────────────────────────────────────┐
│                        CAR                               │
│                                                          │
│   USB-A Port ◄──── USB Cable ────► [MAIN UNIT]          │
│   (Head Unit)                      RPi Zero 2W          │
│                                    ┌──────────┐         │
│                                    │ Wi-Fi AP │◄──┐     │
│                                    │ BT 4.2   │   │     │
│                                    │ USB Gadget│   │     │
│                                    └──────────┘   │     │
│                                         ▲         │     │
│                                    Wi-Fi│    Wi-Fi│     │
│                                         │         │     │
│                                    ┌────┘    ┌────┘     │
│                                    │         │          │
│                               [PHONE]   [ESP32-CAM]     │
│                               iPhone/   Baby Monitor    │
│                               Android   Camera Module   │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### Module 1: Main Unit — Raspberry Pi Zero 2W (~$15)

**Why RPi Zero 2W:**
- Quad-core ARM Cortex-A53, 512 MB RAM — runs full Linux
- Built-in 802.11ac (5 GHz) + Bluetooth 4.2
- USB OTG — can act as a USB gadget (the car sees it as a phone)
- Proven platform: [WirelessAndroidAutoDongle](https://github.com/nisargjhaveri/WirelessAndroidAutoDongle) already works on this exact board
- Tiny: 65mm x 30mm — fits in a small enclosure
- Low power: ~1W, easily powered by the car's USB port

**What it does:**
- Appears to the car's head unit as a wired Android Auto / CarPlay device via USB
- Runs a Wi-Fi hotspot that the phone connects to wirelessly
- Bridges: Car USB ↔ Wi-Fi ↔ Phone
- Receives the ESP32-CAM video stream and composites it for split-screen

### Module 2: Baby Camera — ESP32-CAM (~$8)

**Why ESP32-CAM:**
- Built-in OV2640 camera + Wi-Fi
- Streams MJPEG over HTTP at 640x480 @ 15-25 fps
- Ultra low cost and tiny form factor
- Can be powered by the car's 12V via a small buck converter or USB
- IR LED variants available for night vision

**What it does:**
- Connects to the RPi's Wi-Fi network as a client
- Streams MJPEG video via HTTP to the RPi
- Optional: I2S microphone (INMP441) for audio monitoring

---

## CarPlay vs Android Auto: Different Difficulty Levels

### Android Auto — FEASIBLE (Open Source Exists)

The [WirelessAndroidAutoDongle](https://github.com/nisargjhaveri/WirelessAndroidAutoDongle) project provides a **working, prebuilt solution** on RPi Zero 2W:
- Uses Linux USB gadget framework to appear as a wired AA device
- Runs hostapd for Wi-Fi AP
- Phone connects wirelessly, car gets wired AA
- No proprietary hardware needed
- Active community, prebuilt images available

### CarPlay — REQUIRES A CARLINKIT DONGLE ($30-50)

Apple CarPlay requires **MFi authentication** — a hardware crypto chip that Apple only provides to licensed manufacturers. There is NO open-source implementation of this.

**Two options:**

| Option | Approach | Cost | Complexity |
|---|---|---|---|
| A: Carlinkit shim | Buy a Carlinkit dongle, use it as auth passthrough via [node-carplay](https://github.com/rhysmorgan134/node-CarPlay) or [FastCarPlay](https://github.com/niellun/FastCarPlay) running on the RPi | +$35-50 | Medium |
| B: Android Auto only | Skip CarPlay, build for Android Auto only | $0 extra | Low |

---

## Bill of Materials

### Minimum Build (Android Auto only + Baby Monitor)

| Component | Purpose | Est. Cost |
|---|---|---|
| Raspberry Pi Zero 2W | Main processing unit | $15 |
| ESP32-CAM (AI-Thinker) | Baby monitor camera | $8 |
| Micro-USB to USB-A cable | RPi to car head unit | $5 |
| MicroSD card (8 GB+) | RPi OS storage | $5 |
| 3D printed enclosure | Housing | $5 |
| Buck converter (12V→5V) or USB splitter | Power for ESP32-CAM | $3 |
| **Total** | | **~$41** |

### Full Build (CarPlay + Android Auto + Baby Monitor)

| Component | Purpose | Est. Cost |
|---|---|---|
| All of the above | — | $41 |
| Carlinkit 4.0 USB dongle | Apple MFi authentication | $40 |
| USB hub (if needed) | Connect Carlinkit to RPi | $5 |
| **Total** | | **~$86** |

### Optional Add-ons

| Component | Purpose | Est. Cost |
|---|---|---|
| IR LED board | Night vision for baby cam | $3 |
| INMP441 I2S mic | Audio monitoring | $3 |
| Wide-angle lens (120°+) | Better camera coverage | $2 |
| Larger PSRAM ESP32-S3-CAM | Better camera performance | $12 |

---

## Software Stack

### RPi Zero 2W (Main Unit)

```
┌─────────────────────────────────────────┐
│           Custom Buildroot Linux         │
├─────────────────────────────────────────┤
│  USB Gadget Driver (configfs)           │  ← Appears as phone to car
│  hostapd (Wi-Fi AP, 5 GHz)             │  ← Phone + ESP32-CAM connect here
│  dnsmasq (DHCP)                         │  ← IP assignment
│  wpa_supplicant (BT management)         │  ← Phone discovery
├─────────────────────────────────────────┤
│  Android Auto Proxy                     │  ← WirelessAndroidAutoDongle
│  (USB gadget ↔ TCP/Wi-Fi bridge)        │
├─────────────────────────────────────────┤
│  CarPlay Proxy (optional)               │  ← FastCarPlay + Carlinkit dongle
│  (Carlinkit USB ↔ car USB bridge)       │
├─────────────────────────────────────────┤
│  Baby Monitor Service                   │  ← Custom: MJPEG proxy/overlay
│  (Pulls MJPEG from ESP32-CAM,           │
│   composites split-screen or            │
│   provides phone-accessible stream)     │
└─────────────────────────────────────────┘
```

### ESP32-CAM (Baby Camera)

```
┌─────────────────────────────────────────┐
│           ESP-IDF / Arduino Framework    │
├─────────────────────────────────────────┤
│  Wi-Fi STA (connects to RPi AP)         │
│  MJPEG HTTP streaming server            │
│  OV2640 camera driver                   │
│  Optional: I2S mic → audio stream       │
│  Optional: IR LED control (GPIO)        │
└─────────────────────────────────────────┘
```

---

## Split-Screen Baby Monitor: How It Works

The split-screen feature has multiple possible approaches:

### Option A: Phone App Approach (Recommended)

The RPi re-exposes the ESP32-CAM's MJPEG stream on the Wi-Fi network. A companion phone app (or just a browser tab) displays the baby cam. The user splits screen on their phone between Android Auto/CarPlay and the baby cam app.

- **Pros**: Simple, no video compositing needed on RPi, phone handles rendering
- **Cons**: Requires phone split-screen or picture-in-picture support

### Option B: Overlay onto CarPlay/AA Stream (Advanced)

Intercept the H.264 video stream being sent to the car, composite the baby cam feed as a picture-in-picture overlay, re-encode, and forward to the car.

- **Pros**: True split-screen on the car display, no phone interaction needed
- **Cons**: Requires H.264 decode → composite → re-encode on the RPi (CPU intensive), adds latency, complex to implement

### Option C: Dedicated Display

Add a small screen (3.5" TFT) to the RPi that shows the baby cam independently.

- **Pros**: Always visible, simple
- **Cons**: Requires mounting a separate screen, adds cost

---

## Development Phases

### Phase 1: Wireless Android Auto Dongle (Week 1-2)
1. Flash WirelessAndroidAutoDongle image to RPi Zero 2W
2. Test with car USB port — verify car sees it as wired AA device
3. Test wireless connection from Android phone
4. Validate end-to-end: phone → Wi-Fi → RPi → USB → car head unit

### Phase 2: ESP32-CAM Baby Monitor (Week 2-3)
1. Flash ESP32-CAM with MJPEG streaming firmware
2. Configure as Wi-Fi station connecting to RPi's AP
3. Verify MJPEG stream accessible from phone browser
4. Add IR LED support for night vision (if using IR variant)
5. Optimize resolution/framerate (target: 640x480 @ 15fps minimum)

### Phase 3: Split-Screen Integration (Week 3-4)
1. Write a lightweight proxy service on RPi that re-serves the ESP32-CAM stream
2. Build a simple companion app or web page for split-screen viewing
3. Test picture-in-picture mode on phone while AA is active

### Phase 4: CarPlay Support (Week 4-6, Optional)
1. Acquire Carlinkit 4.0 dongle
2. Build FastCarPlay or node-carplay on the RPi
3. Implement switching logic between AA and CarPlay modes
4. Test with iPhone

### Phase 5: Enclosure & Polish (Week 6-7)
1. Design 3D-printable enclosure for RPi + connectors
2. Design camera mount for ESP32-CAM (headrest, visor, etc.)
3. Add auto-start on power (car USB provides power on ignition)
4. Power management: clean shutdown on USB power loss

---

## Key Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| RPi Zero 2W availability | Can't build | Orange Pi Zero2 or NanoPi NEO as fallback |
| 5 GHz Wi-Fi congestion in car | Connectivity drops | Use DFS channels, optimize hostapd config |
| Car USB port power insufficient (500mA) | RPi browns out | Use car 12V → 5V buck converter instead |
| ESP32-CAM Wi-Fi range | Camera disconnects | Use external antenna variant, or ESP32-S3 with better radio |
| Apple changes CarPlay auth | Carlinkit stops working | Android Auto path unaffected; Carlinkit community tracks updates |
| Video latency too high for baby monitor | Unusable | Tune MJPEG quality/resolution, use wired fallback |

---

## Alternative: ESP32-P4 Future Path

The ESP32-P4 (released 2024-2025) has hardware H.264 and USB High-Speed, making it the most capable ESP32 for video tasks. However:
- **No built-in Wi-Fi** — requires a companion ESP32-C6 for wireless
- **Cannot run Linux** — still a bare-metal/RTOS chip
- **Ecosystem is immature** — limited community support
- Could potentially handle the Android Auto protocol if someone ports it to ESP-IDF (no one has yet)

The P4 is worth watching for a future all-ESP32 solution, but today the RPi Zero 2W is the pragmatic choice.

---

## Repository Structure (Proposed)

```
espwirelesscar/
├── PROJECT_PLAN.md          ← This file
├── docs/
│   ├── wiring-diagram.md    ← Connection diagrams
│   └── parts-list.md        ← Detailed BOM with links
├── esp32-cam/
│   ├── platformio.ini       ← PlatformIO project config
│   └── src/
│       └── main.cpp         ← Camera streaming firmware
├── rpi/
│   ├── buildroot-overlay/   ← Custom Buildroot overlay files
│   ├── baby-monitor/        ← MJPEG proxy service
│   └── scripts/
│       ├── setup.sh         ← Initial RPi setup
│       └── start.sh         ← Service launcher
├── enclosure/
│   ├── main-unit.stl        ← 3D printable RPi case
│   └── camera-mount.stl     ← Camera mounting bracket
└── app/                     ← Optional companion phone app
    └── baby-monitor-pip/    ← PiP baby cam viewer
```

---

## Open Questions for You

1. **CarPlay support**: Do you need CarPlay (adds ~$40 for Carlinkit dongle + complexity), or is Android Auto only sufficient?
2. **Baby monitor display**: Which split-screen approach do you prefer?
   - A: Phone app/browser (simplest)
   - B: Overlay on car display (hardest but coolest)
   - C: Separate small screen
3. **Camera features**: Do you need night vision (IR) and/or audio monitoring?
4. **Form factor priority**: Smallest possible size, or okay with a slightly larger enclosure for better thermals?
5. **Are you okay with RPi Zero 2W as the main brain**, given that a pure ESP32 solution isn't feasible for CarPlay/AA?

# Wireless Connectivity Architecture

## The BLE Bandwidth Problem

Android Auto / CarPlay streams H.264 video + audio + touch input over USB.
This USB data needs to be tunneled from the ESP32-S3 dongle to the RPi.

| Protocol | Real-world Throughput | AA/CarPlay Needs | Verdict |
|---|---|---|---|
| BLE 4.2 (RPi Zero 2W) | ~700 Kbps (90 KB/s) | 5-10 Mbps | **10-15x too slow** |
| BLE 5.0 (2M PHY) | ~1.4 Mbps | 5-10 Mbps | Still 4-7x too slow |
| Bluetooth Classic (SPP) | ~2-3 Mbps | 5-10 Mbps | Still too slow |
| **Wi-Fi (802.11n)** | **20-40 Mbps** | **5-10 Mbps** | **Works with headroom** |

**BLE cannot carry the USB data tunnel.** It's designed for low-power sensor
data (temperature readings, heart rate), not video streaming. A single
720p video frame is larger than BLE can transfer in a second.

---

## What the RPi Zero 2W Radio Can Do

The RPi Zero 2W uses the **CYW43439** combo chip:
- Wi-Fi 802.11 b/g/n (**2.4 GHz only** — no 5 GHz!)
- Bluetooth 4.2 Classic + BLE
- **Simultaneous Wi-Fi + Bluetooth** — confirmed supported

### Concurrent connections the RPi can handle:

| Connection | Protocol | Purpose | Simultaneous? |
|---|---|---|---|
| Wi-Fi AP → ESP32-S3 | Wi-Fi (2.4 GHz) | USB data tunnel | Yes |
| Wi-Fi AP → Phone | Wi-Fi (2.4 GHz) | Android Auto data | Yes (same AP) |
| BT Classic → Phone | Bluetooth 4.2 | AA/CarPlay discovery & pairing | Yes (w/ Wi-Fi) |
| BLE → ESP32-S3 | BLE 4.2 | Control channel (status, commands) | Yes (w/ Wi-Fi) |

**All four can run simultaneously** on the single CYW43439 chip.

---

## Recommended Architecture: Wi-Fi Data + BLE Control

```
                    ┌─────────────────────────────────────────────┐
                    │           RPi Zero 2W (Wi-Fi AP)            │
                    │          ════════════════════                │
                    │  Wi-Fi AP: "CarBridge" (2.4 GHz 802.11n)    │
                    │  BT Classic: AA/CarPlay phone discovery      │
                    │  BLE: ESP32-S3 control channel               │
                    │  CSI: Camera Module v3                       │
                    └───┬──────────────┬──────────────┬───────────┘
                        │              │              │
                   Wi-Fi│         Wi-Fi│          BT  │
                   Client         Client       Classic│
                        │              │              │
                  ┌─────┴─────┐  ┌─────┴─────┐  ┌────┴────┐
                  │ ESP32-S3  │  │   Phone    │  │  Phone  │
                  │ Dongle    │  │  (Wi-Fi)   │  │  (BT)   │
                  │           │  │            │  │         │
                  │ Wi-Fi STA │  │ AA/CarPlay │  │ Initial │
                  │ + BLE     │  │ data over  │  │ pairing │
                  │           │  │ TCP/Wi-Fi  │  │ only    │
                  └─────┬─────┘  └────────────┘  └─────────┘
                        │
                    USB Device
                        │
                  ┌─────┴─────┐
                  │  Car USB  │
                  │  Head Unit│
                  └───────────┘
```

### Data flow:

1. **Car → ESP32-S3**: Car sends USB data (AA protocol) to ESP32-S3 dongle
2. **ESP32-S3 → RPi (Wi-Fi)**: ESP32-S3 packs USB data into TCP, sends over Wi-Fi
3. **RPi processes**: Runs the AA/CarPlay protocol stack
4. **RPi → Phone (Wi-Fi)**: RPi communicates with phone over same Wi-Fi AP
5. **RPi ← Camera (CSI)**: Camera feeds directly via ribbon cable

### BLE role (low-bandwidth control only):

The BLE link between RPi and ESP32-S3 handles:
- Connection status / heartbeat
- Configuration changes (switch AA/CarPlay mode)
- Power management commands
- Diagnostics / error reporting
- Initial pairing before Wi-Fi credentials are exchanged

This is a nice "management plane" while Wi-Fi carries the "data plane."

---

## The 2.4 GHz Limitation

**Important**: RPi Zero 2W only has 2.4 GHz Wi-Fi.

| Impact | Details |
|---|---|
| Android Auto | Works fine on 2.4 GHz |
| CarPlay | Apple **recommends** 5 GHz but 2.4 GHz can work for wired-to-wireless bridge |
| Bandwidth budget | 802.11n @ 2.4 GHz ≈ 30-40 Mbps real. AA uses ~10 Mbps + USB tunnel ~10 Mbps = ~20 Mbps. Fits within budget. |
| Interference | 2.4 GHz is crowded. In a parking lot near other cars' BT/Wi-Fi, performance may degrade. |

### If 5 GHz is needed (CarPlay or better reliability):

Upgrade from RPi Zero 2W to one of:

| Board | Wi-Fi | Size | Cost | Trade-off |
|---|---|---|---|---|
| RPi Zero 2W | 2.4 GHz only | 65x30mm | $15 | Smallest, cheapest |
| RPi 3A+ | 2.4 + 5 GHz | 65x56mm | $25 | Dual-band, slightly larger |
| RPi 4B (1GB) | 2.4 + 5 GHz | 85x56mm | $35 | Most powerful, largest |
| RPi CM4 + carrier | 2.4 + 5 GHz | Custom | $35+ | Custom form factor possible |

---

## ESP32-S3 Dongle Firmware: Dual-Radio Operation

The ESP32-S3 also has both Wi-Fi and BLE, and can run them simultaneously:

```c
// ESP32-S3 dongle runs:
// 1. USB Device (TinyUSB) — connected to car
// 2. Wi-Fi STA — connected to RPi's AP, carries USB data over TCP
// 3. BLE Server — connected to RPi, carries control messages

// Startup sequence:
// a) Power on (car USB provides power)
// b) Start BLE advertising → RPi discovers dongle
// c) RPi sends Wi-Fi AP credentials over BLE
// d) ESP32-S3 connects to RPi's Wi-Fi AP
// e) TCP tunnel established → USB data flows over Wi-Fi
// f) BLE remains active for status/control
```

This is a clean separation:
- **BLE**: Discovery, pairing, credentials exchange, status monitoring
- **Wi-Fi**: High-bandwidth USB data tunnel (the heavy lifting)

---

## Summary

| Question | Answer |
|---|---|
| Can BLE carry the USB/AA data? | **No** — 10-15x too slow |
| Can Wi-Fi carry it? | **Yes** — plenty of bandwidth |
| Can RPi do BLE + Wi-Fi + BT Classic simultaneously? | **Yes** — CYW43439 supports concurrent operation |
| Can ESP32-S3 do BLE + Wi-Fi simultaneously? | **Yes** — ESP32-S3 supports coexistence |
| Can both phone + dongle connect to RPi's Wi-Fi AP? | **Yes** — standard multi-client AP |
| Does RPi Zero 2W have 5 GHz? | **No** — 2.4 GHz only. Upgrade to RPi 3A+ if needed. |

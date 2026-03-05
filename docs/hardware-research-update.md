# Hardware Research Update — IMX708, Orange Pi Zero 2W, and Alternatives

## TL;DR

| Question | Answer |
|---|---|
| IMX708 camera? | Great sensor, but **does NOT work on Radxa Zero 3W** — driver/ISP broken on RK3566 |
| Orange Pi Zero 2W? | **No CSI port, broken HW video codecs on Linux** — disqualified |
| Better board in this size? | **No.** Radxa Zero 3W is the only 65x30mm board that meets all requirements |
| Best camera for this build? | **Arducam IMX219 NoIR Wide Angle** (8MP, 3.4x zoom at 720p) — confirmed working |

---

## IMX708 Analysis (SainSmart / Arducam / RPi Camera Module 3)

### Specs (impressive)

| Spec | IMX708 | IMX219 (current pick) |
|---|---|---|
| Resolution | 12MP (4608x2592) | 8MP (3280x2464) |
| Digital zoom at 720p | ~3.6x | ~3.4x |
| Autofocus | PDAF (phase detection) — fast | Fixed focus |
| HDR | Yes (up to 3MP) | No |
| Wide angle variant | 120° / 152° | 120° (Arducam wide) |
| NoIR variant | Yes (RPi Camera Module 3 Wide NoIR) | Yes (Arducam IMX219 NoIR) |
| Price | $30-45 | $15-25 |

### The problem: NOT compatible with Radxa Zero 3W

The Radxa Zero 3W (RK3566) only supports **two camera sensors**:
- **IMX219** (RPi Camera v2 / Arducam variants)
- **OV5647** (RPi Camera v1.3)

The IMX708 requires:
1. A kernel driver (exists but broken on RK3566)
2. ISP tuning/IQ files specific to the sensor (don't exist for rockchip rkaiq)
3. Autofocus motor control via libcamera (not supported outside RPi ecosystem)

Community attempts to port IMX708 to RK3566 result in:
- rkaiq ISP engine crashes with segfaults during initialization
- 3A (autofocus, auto-exposure, auto-white-balance) completely non-functional
- Raw data streams but images are unusable

**Bottom line**: The IMX708 is a Raspberry Pi ecosystem sensor. On Rockchip boards, only the IMX219 and OV5647 work.

### Why not just use a Raspberry Pi then?

| Board | CSI | 5 GHz WiFi | HW Video Codecs | USB OTG | Size |
|---|---|---|---|---|---|
| RPi Zero 2W | Yes | **No (2.4 GHz only)** | Partial (VideoCore IV) | Yes | 65x30mm |
| RPi 4B | Yes | Yes | Limited encode | Yes | **85x56mm (too big)** |
| RPi 5 | Yes | Yes | **No HW encode** | Yes | **85x56mm (too big)** |
| Radxa Zero 3W | Yes | Yes | **Full HW encode+decode** | Yes | 65x30mm |

The RPi Zero 2W lacks 5 GHz WiFi (required for CarPlay). The RPi 4/5 are too large
and lack the RK3566's excellent hardware H.264 encode pipeline needed for real-time
compositing. The Radxa Zero 3W is the right board.

---

## Orange Pi Zero 2W Analysis

### Specs

| Spec | Value |
|---|---|
| SoC | Allwinner H618 (4x Cortex-A53 @ 1.5 GHz) |
| RAM | 1/1.5/2/4 GB LPDDR4 |
| WiFi | 802.11ac dual-band (2.4 + 5 GHz) |
| Bluetooth | 5.0 |
| Size | 65x30mm (same as Radxa Zero 3W) |
| Price | ~$20-35 |
| **CSI Camera** | **NO — confirmed absent** |
| **HW Video (Linux)** | **BROKEN — H618 cedrus VPU not working** |
| USB OTG | Partial (reports are mixed) |

### Why it's disqualified

1. **No CSI camera port** — The 24-pin expansion connector carries USB/Ethernet/audio,
   not MIPI CSI. Camera must be USB, which means bulkier modules, no NoIR options,
   and the single USB-C data port is occupied.

2. **Hardware video codecs don't work on Linux** — The H618's cedar VPU has no working
   Linux driver. Community spent months trying; hardware-accelerated H.264
   encode/decode is non-functional. Without this, split-screen compositing in
   real-time is impossible on a Cortex-A53.

3. **USB gadget mode uncertain** — Mixed reports on whether USB OTG gadget mode works
   reliably on the H618.

The 4GB RAM and 5 GHz WiFi are appealing, but without CSI or working video codecs,
this board cannot do what we need.

---

## Every 65x30mm SBC Compared

| Board | SoC | CSI | 5 GHz WiFi | BT 5+ | HW H.264 Encode | USB OTG | RAM | Verdict |
|---|---|---|---|---|---|---|---|---|
| **Radxa Zero 3W** | RK3566 | YES | YES (WiFi 6) | 5.4 | YES (h264_rkmpp) | YES | 1-8 GB | **THE ONE** |
| Radxa Zero 3E | RK3566 | YES | NO (Ethernet only) | NO | YES | YES | 1-8 GB | No WiFi |
| Radxa Zero (orig) | S905Y2 | NO | YES (WiFi 5) | 5.0 | Partial | YES | 0.5-4 GB | No CSI |
| Orange Pi Zero 2W | H618 | NO | YES (WiFi 5) | 5.0 | BROKEN | Partial | 1-4 GB | No CSI, no VPU |
| Banana Pi M2 Zero | H3 | YES | NO (2.4 GHz) | YES | Partial | YES | 512 MB | No 5 GHz, low RAM |
| Banana Pi M4 Zero | H618 | NO* | YES (WiFi 5) | 4.2 | BROKEN | YES | 2 GB | No real CSI, no VPU |
| Radxa Cubie A7Z | A733 | YES | YES (WiFi 6) | 5.4 | HW present, **driver WIP** | YES | 1-16 GB | Future (2027?) |
| LuckFox Pico Ultra W | RV1106 | YES | NO (2.4 GHz) | 5.2 | ISP only | NO | 256 MB | Too small/limited |

**The Radxa Zero 3W is the only board that checks every box.**

The Radxa Cubie A7Z (Allwinner A733) is worth watching — same form factor, WiFi 6,
BT 5.4, CSI, up to 16 GB RAM — but its VPU driver doesn't exist yet.

---

## Camera: Best Option for This Build

Since we're locked to the Radxa Zero 3W (RK3566), and RK3566 only supports IMX219
and OV5647, the best camera is:

### Arducam IMX219 NoIR Wide Angle — 120° FOV

| Spec | Value |
|---|---|
| Sensor | Sony IMX219 — 8MP (3280x2464) |
| FOV | 120° wide angle (sees entire backseat) |
| Night vision | NoIR — no IR filter, works with 850nm IR LEDs |
| Digital zoom at 720p | ~3.4x with no quality loss |
| Digital zoom at 1080p | ~2.3x with no quality loss |
| Autofocus | No (fixed focus, infinity to ~20cm) |
| Board size | 24x25mm |
| Interface | CSI (22-pin, matches Radxa Zero 3W) |
| Price | ~$15-20 |
| Radxa Zero 3W compatible | Confirmed working (overlay: radxa-zero3-imx219) |

### Zoom breakdown

| Zoom Level | Crop from 3280x2464 | Output | Quality |
|---|---|---|---|
| 1.0x | Full frame | 1280x720 | Excellent (downscaled) |
| 2.0x | 1640x1232 center crop | 1280x720 | Good (slight downscale) |
| 3.0x | 1093x821 center crop | 1280x720 | Good (near 1:1 pixels) |
| 3.4x | 965x724 center crop | 1280x720 | Acceptable (near native) |

At 3.4x zoom on a 120° wide-angle lens, you're cropping from a ~35° effective FOV —
that's tight enough to see a baby's face clearly from a headrest mount at ~2 feet away.

### Why not IMX519 16MP?

The Arducam IMX519 (16MP, PDAF autofocus) is a better sensor on paper, but it's NOT
listed in Radxa's supported camera list. It requires a separate libcamera driver that
hasn't been ported to the RK3566 ISP. Same problem as the IMX708.

On RK3566: **IMX219 is the best camera you can use.** Period.

### Making the most of IMX219

The IMX219 NoIR + IR LEDs at 120° wide angle is genuinely excellent for a baby monitor:
- 120° sees the entire backseat without mounting precision
- 8MP provides clean 3.4x digital zoom
- NoIR + IR LEDs = clear night vision without visible light
- Fixed focus is actually fine — backseat is always 1.5-3 feet away
- The V4L2 crop → RGA scale pipeline on RK3566 handles zoom in hardware, zero CPU

---

## Updated Final Hardware Recommendation

No changes needed from the existing IMPLEMENTATION_PLAN.md. The research confirms
the current component selection is optimal:

| Component | Model | Why |
|---|---|---|
| Brain | **Radxa Zero 3W 1GB** | Only 65x30mm SBC with CSI + 5GHz WiFi + HW codecs + USB OTG |
| Camera | **Arducam IMX219 NoIR Wide Angle** | Only high-res camera confirmed working on RK3566 |
| USB Dongle | **LilyGO T-Dongle-S3** | ESP32-S3, USB-A plug form factor |
| MFi Chip | **Apple MFi coprocessor (salvaged)** | Required for CarPlay auth |
| IR LEDs | **850nm modules** | Night vision illumination |
| Storage | **High-endurance 32GB microSD** | Reliable writes |

### What about upgrading to more RAM?

The Radxa Zero 3W comes in 1/2/4/8 GB variants. For this project:
- **1 GB is sufficient** — trimmed Debian (~200 MB), compositor uses zero-copy DMA buffers
- **2 GB gives comfort margin** — recommended if the price delta is small (~$5 more)
- **4+ GB is overkill** — nothing in this pipeline needs it

# Radxa Cubie A7Z — AADongle Build

## Quick Start

```bash
# Flash official Radxa Debian image, boot, connect via SSH, then:
git clone <this-repo>
cd TinySightAI/radxa/scripts
sudo bash setup.sh
```

## Post-Setup Hardware Configuration

### Radxa Camera 4K (IMX415)

The camera device tree overlay must be enabled once via the interactive `rsetup` tool.
This cannot be automated — it writes to the boot DTB which requires user confirmation.

```bash
sudo rsetup
```

Navigate to: **Overlays** -> **Enable Radxa Camera 4K**

Then reboot. After reboot, verify:

```bash
ls /dev/video*          # should show video0
v4l2-ctl --list-devices # should show IMX415
```

This only needs to be done once per SD card image. The overlay persists across reboots
and survives the minimal rootfs build + read-only conversion.

### NPU (VIP9000)

The NPU requires the Allwinner vendor kernel with `/dev/vipcore` support.
If `setup.sh` reports the kernel driver is missing:

```bash
# Install vendor kernel with NPU support
wget https://github.com/cubie-image/sun55iw3p1/releases/download/linux_deb/linux-image-5.15.147-500-aw2501_5.15.147-500_arm64.deb
sudo dpkg -i linux-image-*.deb
sudo reboot
```

After reboot, verify:

```bash
ls -l /dev/vipcore      # should exist
```

## Build Order

`setup.sh` handles everything automatically:

1. System packages (apt)
2. VIPLite NPU SDK (from github.com/ZIFENG278/ai-sdk)
3. Rust toolchain (rustup)
4. Compositor (C, FFmpeg + VIPLite)
5. CarPlay stack (C, OpenSSL + FFmpeg)
6. aa-proxy (Rust)
7. uStreamer (C)
8. Systemd services + configs

## Minimal Rootfs

After `setup.sh` completes and everything is verified working:

```bash
sudo bash build-minimal-rootfs.sh
```

This creates a ~150MB debootstrap image with only the packages needed to run.

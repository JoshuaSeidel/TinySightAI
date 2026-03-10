#!/bin/bash
# =============================================================================
# AADongle Radxa Zero 3W — Complete Setup Script
# Builds and installs ALL project components from source.
#
# Usage: sudo bash setup.sh
# =============================================================================
set -euo pipefail

# -----------------------------------------------------------------------------
# Auto-detect project root (two levels up from scripts/)
# -----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RADXA_DIR="$PROJECT_DIR/radxa"
CONFIG_DIR="$RADXA_DIR/config"
INSTALL_DIR="/opt/aadongle"

# -----------------------------------------------------------------------------
# Color output helpers
# -----------------------------------------------------------------------------
RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
BLU='\033[0;34m'
CYN='\033[0;36m'
RST='\033[0m'

step()  { echo -e "\n${BLU}==>${RST} ${CYN}$*${RST}"; }
ok()    { echo -e "  ${GRN}OK${RST}  $*"; }
warn()  { echo -e "  ${YLW}WARN${RST} $*"; }
fail()  { echo -e "  ${RED}FAIL${RST} $*" >&2; }
banner(){ echo -e "\n${GRN}================================================================${RST}"; \
          echo -e "${GRN}  $*${RST}"; \
          echo -e "${GRN}================================================================${RST}"; }

# -----------------------------------------------------------------------------
# Preflight checks
# -----------------------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    fail "This script must be run as root.  Use: sudo bash $0"
    exit 1
fi

banner "AADongle Radxa Zero 3W — Full Setup"
echo "  Project root : $PROJECT_DIR"
echo "  Radxa dir    : $RADXA_DIR"
echo "  Install dir  : $INSTALL_DIR"
echo "  Running as   : root ($(whoami))"
echo ""

# Verify we are actually in the right directory tree
if [ ! -d "$RADXA_DIR/compositor" ] || [ ! -d "$RADXA_DIR/carplay" ] || \
   [ ! -d "$RADXA_DIR/aa-proxy" ]; then
    fail "Expected directories not found under $RADXA_DIR."
    fail "Run setup.sh from inside the espwirelesscar repository."
    exit 1
fi

# Keep track of what we install for the final summary
declare -a SUMMARY_OK=()
declare -a SUMMARY_WARN=()

# Helper: record success / warning
record_ok()   { SUMMARY_OK+=("$*"); }
record_warn() { SUMMARY_WARN+=("$*"); }

# =============================================================================
# STEP 1 — System packages
# =============================================================================
step "[1/10] Updating apt package index..."
apt-get update -qq
ok "apt index updated"

step "[2/10] Installing all required system packages..."
apt-get install -y \
    `# Core system tools` \
    build-essential git curl wget \
    `# Networking / WiFi AP` \
    hostapd dnsmasq-base \
    `# Bluetooth` \
    bluez \
    `# Camera / V4L2` \
    v4l-utils \
    `# GPIO` \
    gpiod \
    `# i2c tools (MFi chip)` \
    i2c-tools \
    `# Build tools` \
    cmake pkg-config nasm \
    `# MPP / RGA (Rockchip hardware codecs)` \
    librockchip-mpp-dev librockchip-mpp1 \
    librga-dev librga2 \
    libdrm-dev \
    `# Compositor link dependencies` \
    libpthread-stubs0-dev \
    `# CarPlay stack` \
    libssl-dev \
    libavahi-client-dev libavahi-common-dev \
    libbluetooth-dev \
    libdbus-1-dev \
    libasound2-dev \
    libavcodec-dev libavutil-dev \
    `# avahi daemon (CarPlay mDNS)` \
    avahi-daemon \
    `# ustreamer build deps` \
    libevent-dev libjpeg62-turbo-dev libbsd-dev \
    `# Python (bt-agent, baby-monitor server, ota-server)` \
    python3 python3-dbus python3-gi gir1.2-glib-2.0 \
    `# Miscellaneous runtime helpers` \
    net-tools iproute2 netcat-openbsd \
    2>&1 | grep -E '(Setting up|already installed|E:)' || true

ok "All system packages installed"
record_ok "System packages"

# Immediately stop hostapd/dnsmasq if apt postinst auto-started them.
# They depend on the ap0 virtual interface which doesn't exist yet.
# They will start properly on reboot via ap0-setup.service dependency.
systemctl stop hostapd 2>/dev/null || true
systemctl stop dnsmasq 2>/dev/null || true
ok "Stopped hostapd/dnsmasq (will start after reboot when ap0 exists)"

# =============================================================================
# STEP 2 — Rust / Cargo
# =============================================================================
step "[3/10] Checking Rust toolchain..."
if ! command -v cargo &>/dev/null; then
    echo "  Rust not found — installing via rustup..."
    # Install into the root account's home; cargo will be at /root/.cargo/bin/cargo
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
    ok "rustup installed"
else
    echo "  Rust already present: $(cargo --version)"
fi

# Source cargo env so it is available for the rest of this script session
# shellcheck source=/dev/null
CARGO_ENV="${HOME}/.cargo/env"
if [ -f "$CARGO_ENV" ]; then
    # shellcheck disable=SC1090
    source "$CARGO_ENV"
fi

# Verify cargo is now available
if ! command -v cargo &>/dev/null; then
    fail "cargo not found after rustup install.  Add ~/.cargo/bin to PATH and re-run."
    exit 1
fi
ok "Rust $(rustc --version), Cargo $(cargo --version)"
record_ok "Rust toolchain"

# =============================================================================
# STEP 3 — Install directory structure
# =============================================================================
step "[4/10] Creating /opt/aadongle directory tree..."
mkdir -p \
    "$INSTALL_DIR/bin" \
    "$INSTALL_DIR/baby-monitor" \
    "$INSTALL_DIR/ota-server" \
    "$INSTALL_DIR/firmware" \
    "$INSTALL_DIR/config" \
    "$INSTALL_DIR/scripts"
ok "Directory tree created under $INSTALL_DIR"

# =============================================================================
# STEP 4 — Network setup
# =============================================================================
step "[5/10] Configuring networking (WiFi AP + DHCP)..."

# Tell NetworkManager to leave the virtual AP interface alone (hostapd manages it)
# wlan0 stays managed by NetworkManager for dev SSH access
mkdir -p /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/aadongle-unmanaged.conf <<'NM_EOF'
[keyfile]
unmanaged-devices=interface-name:ap0
NM_EOF
ok "ap0 marked unmanaged by NetworkManager (wlan0 stays managed for dev SSH)"

# Create a service to set up the virtual AP interface and assign static IP
cat > /etc/systemd/system/ap0-setup.service <<'UNIT_EOF'
[Unit]
Description=Create virtual AP interface (ap0) and set static IP
Before=hostapd.service
After=sys-subsystem-net-devices-wlan0.device
Wants=sys-subsystem-net-devices-wlan0.device

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/iw dev wlan0 interface add ap0 type __ap
ExecStart=/sbin/ip addr add 192.168.4.1/24 dev ap0
ExecStart=/sbin/ip link set ap0 up
ExecStop=/sbin/iw dev ap0 del

[Install]
WantedBy=multi-user.target
UNIT_EOF
systemctl enable ap0-setup.service
ok "ap0 virtual AP service created (192.168.4.1/24, wlan0 free for SSH)"

# hostapd — configure but do NOT start (ap0 doesn't exist until reboot)
if [ -f "$CONFIG_DIR/hostapd.conf" ]; then
    install -m 640 "$CONFIG_DIR/hostapd.conf" /etc/hostapd/hostapd.conf
    # Ensure /etc/default/hostapd points at our config file
    if [ -f /etc/default/hostapd ]; then
        sed -i 's|^#*DAEMON_CONF=.*|DAEMON_CONF="/etc/hostapd/hostapd.conf"|' \
            /etc/default/hostapd
    fi
    # Drop-in: hostapd must wait for ap0 interface to exist
    mkdir -p /etc/systemd/system/hostapd.service.d
    cat > /etc/systemd/system/hostapd.service.d/wait-for-ap0.conf <<'DROP_EOF'
[Unit]
Requires=ap0-setup.service
After=ap0-setup.service
DROP_EOF
    systemctl unmask hostapd 2>/dev/null || true
    systemctl enable hostapd
    systemctl stop hostapd 2>/dev/null || true
    ok "hostapd configured and enabled (will start after reboot with ap0)"
    record_ok "hostapd (hidden 5GHz AP)"
else
    warn "hostapd.conf not found at $CONFIG_DIR/hostapd.conf — skipping"
    record_warn "hostapd config missing"
fi

# dnsmasq — configure but do NOT start (binds to ap0 which doesn't exist yet)
if [ -f "$CONFIG_DIR/dnsmasq.conf" ]; then
    install -m 644 "$CONFIG_DIR/dnsmasq.conf" /etc/dnsmasq.d/aadongle.conf
    # Drop-in: dnsmasq must wait for ap0 interface to exist
    mkdir -p /etc/systemd/system/dnsmasq.service.d
    cat > /etc/systemd/system/dnsmasq.service.d/wait-for-ap0.conf <<'DROP_EOF'
[Unit]
Requires=ap0-setup.service
After=ap0-setup.service
DROP_EOF
    systemctl enable dnsmasq
    systemctl stop dnsmasq 2>/dev/null || true
    ok "dnsmasq configured and enabled (will start after reboot with ap0)"
    record_ok "dnsmasq (DHCP server)"
else
    warn "dnsmasq.conf not found at $CONFIG_DIR/dnsmasq.conf — skipping"
    record_warn "dnsmasq config missing"
fi

# IP forwarding (needed if we ever NAT through to the car's network)
if ! grep -q "^net.ipv4.ip_forward=1" /etc/sysctl.d/99-aadongle.conf 2>/dev/null; then
    echo "net.ipv4.ip_forward=1" >> /etc/sysctl.d/99-aadongle.conf
fi
sysctl -w net.ipv4.ip_forward=1 >/dev/null
ok "IP forwarding enabled"

# Safety: ensure NetworkManager + wpa_supplicant are still running (dev SSH access)
systemctl is-active NetworkManager >/dev/null 2>&1 || systemctl start NetworkManager
systemctl is-active wpa_supplicant >/dev/null 2>&1 || systemctl start wpa_supplicant
ok "NetworkManager + wpa_supplicant verified running (dev SSH preserved)"

# =============================================================================
# STEP 5 — Bluetooth
# =============================================================================
step "[5b/10] Configuring Bluetooth..."
if [ -f "$CONFIG_DIR/bluetooth.conf" ]; then
    install -m 644 "$CONFIG_DIR/bluetooth.conf" /etc/bluetooth/main.conf
    ok "bluetooth.conf installed"
    record_ok "Bluetooth config"
else
    warn "bluetooth.conf not found at $CONFIG_DIR/bluetooth.conf — skipping"
    record_warn "Bluetooth config missing"
fi
systemctl enable bluetooth
ok "bluetooth.service enabled"

# =============================================================================
# STEP 6 — Avahi daemon (CarPlay mDNS / Bonjour)
# =============================================================================
step "[5c/10] Enabling avahi-daemon for CarPlay mDNS..."
systemctl enable avahi-daemon
systemctl start avahi-daemon 2>/dev/null || true
ok "avahi-daemon enabled (required for CarPlay Bonjour advertisement)"
record_ok "avahi-daemon (CarPlay mDNS)"

# =============================================================================
# STEP 7 — Camera overlay
# =============================================================================
step "[6/10] Configuring IMX219 camera overlay..."
if [ -f /boot/dtbo/radxa-zero3-imx219.dtbo ]; then
    if ! grep -q "radxa-zero3-imx219" /boot/uEnv.txt 2>/dev/null; then
        echo "overlays=radxa-zero3-imx219" >> /boot/uEnv.txt
        ok "IMX219 overlay added to /boot/uEnv.txt (reboot required)"
    else
        ok "IMX219 overlay already present in /boot/uEnv.txt"
    fi
    record_ok "IMX219 camera overlay"
else
    warn "IMX219 dtbo not found at /boot/dtbo/radxa-zero3-imx219.dtbo"
    warn "Run 'sudo rsetup' → Overlays → Enable radxa-zero3-imx219 after setup"
    record_warn "IMX219 overlay: set manually via rsetup"
fi

# Ensure v4l2 modules are loaded at boot
if ! grep -q "v4l2_common" /etc/modules 2>/dev/null; then
    printf 'v4l2_common\nvideobuf2_common\n' >> /etc/modules
fi
ok "v4l2 modules registered"

# =============================================================================
# STEP 8 — Build software
# =============================================================================

# -----------------------------------------------------------------------
# 8a. Compositor (C, links to librockchip_mpp + librga + libdrm)
# -----------------------------------------------------------------------
step "[7/10] Building compositor..."
cd "$RADXA_DIR/compositor"
make clean
if make -j"$(nproc)"; then
    make install   # installs to /opt/aadongle/bin/compositor
    ok "compositor built and installed to $INSTALL_DIR/bin/compositor"
    record_ok "compositor binary"
else
    fail "compositor build failed — check $RADXA_DIR/compositor/src for errors"
    record_warn "compositor: BUILD FAILED"
fi

# -----------------------------------------------------------------------
# 8b. CarPlay stack (C, iAP2 + AirPlay)
# -----------------------------------------------------------------------
step "[7/10] Building CarPlay stack..."
cd "$RADXA_DIR/carplay"
make clean
if make -j"$(nproc)"; then
    make install   # installs to /opt/aadongle/bin/carplay-stack
    ok "carplay-stack built and installed to $INSTALL_DIR/bin/carplay-stack"
    record_ok "carplay-stack binary"
else
    fail "carplay-stack build failed — check $RADXA_DIR/carplay/src for errors"
    record_warn "carplay-stack: BUILD FAILED"
fi

# -----------------------------------------------------------------------
# 8c. AA proxy (Rust)
# -----------------------------------------------------------------------
step "[7/10] Building aa-proxy (Rust)..."
cd "$RADXA_DIR/aa-proxy"
if cargo build --release 2>&1; then
    install -m 755 target/release/aa-proxy "$INSTALL_DIR/bin/aa-proxy"
    ok "aa-proxy built and installed to $INSTALL_DIR/bin/aa-proxy"
    record_ok "aa-proxy binary (Rust)"
else
    fail "aa-proxy cargo build failed"
    record_warn "aa-proxy: BUILD FAILED"
fi

# -----------------------------------------------------------------------
# 8d. ustreamer (MJPEG camera server for baby-monitor web UI)
# -----------------------------------------------------------------------
step "[7/10] Building ustreamer..."
if bash "$SCRIPT_DIR/build-ustreamer.sh"; then
    ok "ustreamer built and installed to $INSTALL_DIR/bin/ustreamer"
    record_ok "ustreamer binary"
else
    warn "ustreamer build failed (non-fatal) — baby-monitor camera stream unavailable"
    record_warn "ustreamer: BUILD FAILED"
fi

# =============================================================================
# STEP 9 — Install non-compiled files
# =============================================================================
step "[8/10] Installing scripts and web assets..."

# BT agent (Python)
install -m 755 "$SCRIPT_DIR/bt-agent.py"           "$INSTALL_DIR/bin/bt-agent.py"
ok "bt-agent.py installed"

# IR LED control script
install -m 755 "$SCRIPT_DIR/ir-led-control.sh"     "$INSTALL_DIR/bin/ir-led-control.sh"
ok "ir-led-control.sh installed"

# Power manager script
# The power-manager.service ExecStart points to /opt/aadongle/scripts/power-manager.sh
mkdir -p "$INSTALL_DIR/scripts"
install -m 755 "$SCRIPT_DIR/power-manager.sh"      "$INSTALL_DIR/scripts/power-manager.sh"
ok "power-manager.sh installed"

# Baby monitor web UI + Python API server
if [ -d "$RADXA_DIR/baby-monitor" ]; then
    cp -r "$RADXA_DIR/baby-monitor/"* "$INSTALL_DIR/baby-monitor/"
    ok "baby-monitor web assets installed"
    record_ok "baby-monitor web UI"
else
    warn "baby-monitor directory not found at $RADXA_DIR/baby-monitor"
    record_warn "baby-monitor: source directory missing"
fi

# OTA server
if [ -f "$RADXA_DIR/ota-server/ota_server.py" ]; then
    cp "$RADXA_DIR/ota-server/ota_server.py" "$INSTALL_DIR/ota-server/"
    ok "ota_server.py installed"
    record_ok "OTA server"
else
    warn "ota_server.py not found at $RADXA_DIR/ota-server/ota_server.py"
    record_warn "OTA server: source file missing"
fi

# Firmware staging directory (populated by OTA process)
mkdir -p "$INSTALL_DIR/firmware"
ok "firmware/ directory ready"

# Config web server + templates
mkdir -p "$INSTALL_DIR/web/templates"
if [ -d "$RADXA_DIR/web" ]; then
    cp "$RADXA_DIR/web/config_server.py" "$INSTALL_DIR/web/"
    cp "$RADXA_DIR/web/templates/"*.html "$INSTALL_DIR/web/templates/" 2>/dev/null || true
    ok "config web server installed"
    record_ok "Config web server"
else
    warn "web/ directory not found at $RADXA_DIR/web"
    record_warn "Config web server: source missing"
fi

# =============================================================================
# STEP 10 — Systemd services
# =============================================================================
step "[9/10] Installing and enabling systemd services..."

SERVICES=(
    aa-bridge
    aa-proxy
    carplay
    baby-monitor
    baby-monitor-api
    bt-agent
    ir-leds
    ota-server
    power-manager
    config-server
)

SYSTEMD_SRC="$CONFIG_DIR/systemd"

# Also install the aadongle.target
if [ -f "${SYSTEMD_SRC}/aadongle.target" ]; then
    cp "${SYSTEMD_SRC}/aadongle.target" /etc/systemd/system/
    ok "Installed aadongle.target"
fi
for svc in "${SERVICES[@]}"; do
    svc_file="${SYSTEMD_SRC}/${svc}.service"
    if [ -f "$svc_file" ]; then
        cp "$svc_file" /etc/systemd/system/
        ok "Installed ${svc}.service"
    else
        warn "${svc}.service not found at $svc_file — skipping"
        record_warn "systemd/${svc}.service: missing"
    fi
done

systemctl daemon-reload
ok "systemd daemon reloaded"

for svc in "${SERVICES[@]}"; do
    if [ -f "/etc/systemd/system/${svc}.service" ]; then
        systemctl enable "${svc}.service" 2>/dev/null && ok "Enabled ${svc}.service" || \
            warn "Could not enable ${svc}.service"
    fi
done

record_ok "systemd services installed and enabled"

# =============================================================================
# STEP 11 — OS optimizations (safe subset, no avahi removal)
# =============================================================================
step "[10/10] Applying OS optimizations (trim-os.sh)..."
if bash "$SCRIPT_DIR/trim-os.sh"; then
    ok "OS trim complete"
    record_ok "OS optimizations (trim-os.sh)"
else
    warn "trim-os.sh exited with errors (non-fatal)"
    record_warn "OS trim: some steps may have failed"
fi

# Install volatile journald config
if [ -f "$CONFIG_DIR/journald-volatile.conf" ]; then
    mkdir -p /etc/systemd/journald.conf.d
    cp "$CONFIG_DIR/journald-volatile.conf" /etc/systemd/journald.conf.d/volatile.conf
    ok "Journald volatile config installed"
fi

# Install overlayfs initramfs scripts (for make-readonly.sh)
if [ -d "$CONFIG_DIR/overlayfs" ]; then
    mkdir -p /etc/initramfs-tools/scripts/init-bottom
    mkdir -p /etc/initramfs-tools/hooks
    cp "$CONFIG_DIR/overlayfs/overlay-init" /etc/initramfs-tools/scripts/init-bottom/overlay-root
    cp "$CONFIG_DIR/overlayfs/overlay-hook" /etc/initramfs-tools/hooks/overlay-root
    chmod 755 /etc/initramfs-tools/scripts/init-bottom/overlay-root
    chmod 755 /etc/initramfs-tools/hooks/overlay-root
    ok "Overlayfs initramfs scripts installed (run make-readonly.sh to activate)"
    record_ok "Read-only rootfs scripts prepared"
fi

# =============================================================================
# FINAL SUMMARY
# =============================================================================
banner "Setup Complete"
echo ""
echo -e "${GRN}Successfully completed:${RST}"
for item in "${SUMMARY_OK[@]}"; do
    echo -e "  ${GRN}+${RST} $item"
done

if [ ${#SUMMARY_WARN[@]} -gt 0 ]; then
    echo ""
    echo -e "${YLW}Warnings / items needing attention:${RST}"
    for item in "${SUMMARY_WARN[@]}"; do
        echo -e "  ${YLW}!${RST} $item"
    done
fi

echo ""
echo -e "${CYN}Installed layout:${RST}"
echo "  $INSTALL_DIR/bin/compositor         — display compositor (AA+CarPlay+cam)"
echo "  $INSTALL_DIR/bin/carplay-stack      — CarPlay iAP2/AirPlay daemon"
echo "  $INSTALL_DIR/bin/aa-proxy           — Android Auto MITM proxy (Rust)"
echo "  $INSTALL_DIR/bin/ustreamer          — MJPEG camera stream server"
echo "  $INSTALL_DIR/bin/bt-agent.py        — Bluetooth NoInputNoOutput pairing agent"
echo "  $INSTALL_DIR/bin/ir-led-control.sh  — IR LED auto-brightness controller"
echo "  $INSTALL_DIR/scripts/power-manager.sh — watchdog + power monitor"
echo "  $INSTALL_DIR/baby-monitor/          — web UI + API server"
echo "  $INSTALL_DIR/ota-server/            — OTA firmware update server"
echo "  $INSTALL_DIR/firmware/              — OTA firmware staging area"
echo ""
echo -e "${CYN}Next steps:${RST}"
echo "  1. Reboot so camera overlay, IP forwarding, and module changes take effect."
echo "  2. If IMX219 overlay was not auto-applied: sudo rsetup → Overlays → radxa-zero3-imx219"
echo "  3. Connect T-Dongle-S3 to car USB-A, phone to hidden SSID 'AADongle'."
echo "  4. Monitor services: journalctl -fu aa-bridge  |  journalctl -fu carplay"
echo "  5. Check WiFi AP: iw dev ap0 info  |  hostapd_cli status"
echo "  6. Optional: sudo bash $SCRIPT_DIR/install-mpp.sh   (builds ffmpeg-rockchip)"
echo "  7. For production: sudo bash $SCRIPT_DIR/make-readonly.sh (read-only root, hard power safe)"
echo ""

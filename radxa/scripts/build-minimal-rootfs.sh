#!/bin/bash
# =============================================================================
# build-minimal-rootfs.sh — Create minimal Debian rootfs via debootstrap
#
# Replaces the bloated stock Radxa image (~2GB) with a lean ~150MB rootfs
# containing ONLY what's needed to run the AADongle system.
#
# Prerequisites:
#   1. Running on Radxa Zero 3W with stock Debian image
#   2. setup.sh has already been run (all binaries compiled)
#   3. At least 512MB free RAM (for tmpfs build area)
#
# Usage: sudo bash build-minimal-rootfs.sh [--dry-run | --no-deploy]
#
# Options:
#   --dry-run     Show what would happen, don't actually build
#   --no-deploy   Build the rootfs but don't deploy (stays at /mnt/newroot)
#   (default)     Build AND deploy — rsync over / then force reboot via sysrq
#
# What this does:
#   1. Creates minimal Debian rootfs via debootstrap
#   2. Installs required runtime packages + SSH + sudo + NetworkManager
#   3. Copies vendor kernel modules + firmware from running system
#   4. Installs our pre-built binaries and configs
#   5. Creates radxa user, copies SSH keys + WiFi profiles
#   6. Applies read-only root + overlayfs hardening
#   7. Deploys over live system via rsync + force reboot
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RADXA_DIR="$PROJECT_DIR/radxa"
CONFIG_DIR="$RADXA_DIR/config"
INSTALL_DIR="/opt/aadongle"

NEWROOT="/mnt/newroot"
DEBIAN_SUITE="bookworm"
DEBIAN_MIRROR="http://deb.debian.org/debian"
RADXA_REPO="https://radxa-repo.github.io/bookworm"
RADXA_KEYRING="/usr/share/keyrings/radxa-archive-keyring-2022.gpg"

DRY_RUN=false
[ "${1:-}" = "--dry-run" ] && DRY_RUN=true

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; CYN='\033[0;36m'; RST='\033[0m'
step()  { echo -e "\n${CYN}==> $*${RST}"; }
ok()    { echo -e "  ${GRN}OK${RST}  $*"; }
warn()  { echo -e "  ${YLW}WARN${RST} $*"; }
fail()  { echo -e "  ${RED}FAIL${RST} $*" >&2; }
die()   { fail "$@"; exit 1; }

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

if [ "$(id -u)" -ne 0 ]; then
    die "Must run as root: sudo bash $0"
fi

if ! command -v debootstrap &>/dev/null; then
    echo "Installing debootstrap..."
    apt-get update -qq && apt-get install -y debootstrap
fi

# Verify binaries exist (setup.sh must have run)
for bin in compositor aa-proxy carplay-stack ustreamer; do
    if [ ! -f "$INSTALL_DIR/bin/$bin" ]; then
        die "$INSTALL_DIR/bin/$bin not found — run setup.sh first"
    fi
done

KERNEL_VER=$(uname -r)
echo ""
echo "=== AADongle Minimal Rootfs Builder ==="
echo "  Kernel:     $KERNEL_VER"
echo "  Target:     $NEWROOT"
echo "  Suite:      $DEBIAN_SUITE"
echo "  Dry run:    $DRY_RUN"
echo ""

if $DRY_RUN; then
    echo "[DRY RUN] Would build minimal rootfs. Exiting."
    exit 0
fi

# ---------------------------------------------------------------------------
# Step 1: Debootstrap minimal base
# ---------------------------------------------------------------------------
step "[1/9] Creating minimal Debian base via debootstrap..."

# Clean any previous attempt
[ -d "$NEWROOT" ] && rm -rf "$NEWROOT"
mkdir -p "$NEWROOT"

debootstrap --variant=minbase \
    --include=systemd,systemd-sysv,dbus,udev,kmod,initramfs-tools,apt,gpg,ca-certificates \
    "$DEBIAN_SUITE" "$NEWROOT" "$DEBIAN_MIRROR"

ok "Base system created ($(du -sh "$NEWROOT" | cut -f1))"

# ---------------------------------------------------------------------------
# Step 2: Add Radxa apt repository
# ---------------------------------------------------------------------------
step "[2/9] Adding Radxa apt repository for vendor packages..."

# Mount /proc /sys /dev for chroot operations
mount --bind /proc "$NEWROOT/proc"
mount --bind /sys "$NEWROOT/sys"
mount --bind /dev "$NEWROOT/dev"
mount --bind /dev/pts "$NEWROOT/dev/pts"

# Cleanup function
cleanup_mounts() {
    umount "$NEWROOT/dev/pts" 2>/dev/null || true
    umount "$NEWROOT/dev" 2>/dev/null || true
    umount "$NEWROOT/sys" 2>/dev/null || true
    umount "$NEWROOT/proc" 2>/dev/null || true
}
trap cleanup_mounts EXIT

# Copy all Radxa GPG keys from host first
FOUND_KEY=false
for keydir in /usr/share/keyrings /etc/apt/trusted.gpg.d /etc/apt/keyrings; do
    if ls "$keydir"/radxa* 2>/dev/null | head -1 >/dev/null; then
        mkdir -p "$NEWROOT/$keydir"
        cp "$keydir"/radxa* "$NEWROOT/$keydir/"
        ok "Copied Radxa GPG keys from $keydir"
        FOUND_KEY=true
    fi
done
$FOUND_KEY || warn "No Radxa GPG key found on host"

# Copy Radxa repo config from host — search all sources files for "radxa"
HOST_RADXA_SRC=$(grep -rl "radxa" /etc/apt/sources.list.d/ 2>/dev/null | head -1)
if [ -n "$HOST_RADXA_SRC" ]; then
    cp "$HOST_RADXA_SRC" "$NEWROOT/etc/apt/sources.list.d/"
    ok "Copied Radxa repo config from host ($HOST_RADXA_SRC)"
else
    # Fallback: write with correct GitHub Pages URL + signed-by
    cat > "$NEWROOT/etc/apt/sources.list.d/radxa.list" << EOF
deb [signed-by=$RADXA_KEYRING] $RADXA_REPO $DEBIAN_SUITE main
EOF
    ok "Radxa repo configured (fallback: $RADXA_REPO)"
fi

# Also keep standard Debian repo
cat > "$NEWROOT/etc/apt/sources.list" << EOF
deb $DEBIAN_MIRROR $DEBIAN_SUITE main
deb $DEBIAN_MIRROR $DEBIAN_SUITE-updates main
deb http://security.debian.org/debian-security $DEBIAN_SUITE-security main
EOF

chroot "$NEWROOT" apt-get update -qq
ok "Radxa apt repo configured"

# ---------------------------------------------------------------------------
# Step 3: Install runtime packages
# ---------------------------------------------------------------------------
step "[3/9] Installing runtime packages (only what we need)..."

# Prevent services from auto-starting during package installation in chroot
mkdir -p "$NEWROOT/usr/sbin"
cat > "$NEWROOT/usr/sbin/policy-rc.d" << 'RCEOF'
#!/bin/sh
exit 101
RCEOF
chmod +x "$NEWROOT/usr/sbin/policy-rc.d"

# Standard Debian packages (always available)
chroot "$NEWROOT" apt-get install -y --no-install-recommends \
    `# SSH + sudo + dev access (CRITICAL — without these, can't log in after deploy)` \
    sudo openssh-server \
    `# WiFi STA management (dev SSH over wlan0) + AP virtual interface` \
    network-manager wpasupplicant iw \
    `# Network` \
    hostapd dnsmasq-base iproute2 netcat-openbsd ifupdown \
    `# Bluetooth + mDNS` \
    bluez avahi-daemon \
    `# Camera / GPIO / I2C` \
    v4l-utils gpiod i2c-tools \
    `# DRM` \
    libdrm2 \
    `# CarPlay shared libs` \
    libssl3 \
    libavahi-client3 libavahi-common3 \
    libbluetooth3 libdbus-1-3 \
    libasound2 \
    libavcodec59 libavutil57 \
    `# ustreamer runtime` \
    libevent-2.1-7 libjpeg62-turbo libbsd0 \
    `# Python (bt-agent, baby-monitor, ota, config server)` \
    python3-minimal python3-dbus python3-gi gir1.2-glib-2.0 \
    `# f2fs for /data partition` \
    f2fs-tools \
    `# Deploy + OTA` \
    rsync wget \
    2>&1 | tail -5

# Rockchip vendor packages — try apt first, fall back to copying from host
step "  Installing Rockchip vendor packages..."
if chroot "$NEWROOT" apt-get install -y --no-install-recommends \
    librockchip-mpp1 librga2 2>/dev/null; then
    ok "Rockchip packages installed via apt"
else
    warn "Rockchip packages not in chroot apt — copying libraries from host"
    # Copy MPP libs
    for lib in /usr/lib/aarch64-linux-gnu/librockchip_mpp*; do
        [ -f "$lib" ] && cp -a "$lib" "$NEWROOT/usr/lib/aarch64-linux-gnu/"
    done
    # Copy RGA libs
    for lib in /usr/lib/aarch64-linux-gnu/librga*; do
        [ -f "$lib" ] && cp -a "$lib" "$NEWROOT/usr/lib/aarch64-linux-gnu/"
    done
    # Also check /usr/lib/ directly
    for lib in /usr/lib/librockchip_mpp* /usr/lib/librga*; do
        [ -f "$lib" ] && cp -a "$lib" "$NEWROOT/usr/lib/"
    done
    ok "Rockchip libraries copied from host"
fi

ok "Runtime packages installed ($(chroot "$NEWROOT" dpkg --list | grep '^ii' | wc -l) packages)"

# Clean apt cache in new rootfs
chroot "$NEWROOT" apt-get clean
rm -rf "$NEWROOT/var/lib/apt/lists/"*
rm -rf "$NEWROOT/var/cache/apt/archives/"*
rm -f "$NEWROOT/usr/sbin/policy-rc.d"
ok "Package cache cleaned"

# ---------------------------------------------------------------------------
# Step 4: Copy vendor kernel modules + firmware
# ---------------------------------------------------------------------------
step "[4/9] Copying kernel modules and firmware from running system..."

# Kernel modules
mkdir -p "$NEWROOT/lib/modules"
cp -a "/lib/modules/$KERNEL_VER" "$NEWROOT/lib/modules/"
ok "Kernel modules copied ($KERNEL_VER)"

# Firmware blobs (WiFi, BT)
mkdir -p "$NEWROOT/lib/firmware"
cp -a /lib/firmware/* "$NEWROOT/lib/firmware/" 2>/dev/null || true
ok "Firmware blobs copied"

# ---------------------------------------------------------------------------
# Step 5: Install our binaries and scripts
# ---------------------------------------------------------------------------
step "[5/9] Installing AADongle binaries and scripts..."

# Create directory tree
mkdir -p "$NEWROOT/opt/aadongle/"{bin,scripts,baby-monitor,ota-server,web,firmware,config}

# Binaries
for bin in compositor aa-proxy carplay-stack ustreamer; do
    if [ -f "$INSTALL_DIR/bin/$bin" ]; then
        cp "$INSTALL_DIR/bin/$bin" "$NEWROOT/opt/aadongle/bin/"
        ok "Copied $bin"
    fi
done

# Python scripts
for script in bt-agent.py ir-led-control.sh; do
    if [ -f "$INSTALL_DIR/bin/$script" ]; then
        cp "$INSTALL_DIR/bin/$script" "$NEWROOT/opt/aadongle/bin/"
    fi
done

# Power manager
cp "$INSTALL_DIR/scripts/power-manager.sh" "$NEWROOT/opt/aadongle/scripts/" 2>/dev/null || \
    cp "$SCRIPT_DIR/power-manager.sh" "$NEWROOT/opt/aadongle/scripts/"
chmod +x "$NEWROOT/opt/aadongle/scripts/power-manager.sh"

# Baby monitor web UI
if [ -d "$INSTALL_DIR/baby-monitor" ]; then
    cp -r "$INSTALL_DIR/baby-monitor/"* "$NEWROOT/opt/aadongle/baby-monitor/"
fi

# OTA server
if [ -f "$INSTALL_DIR/ota-server/ota_server.py" ]; then
    cp "$INSTALL_DIR/ota-server/ota_server.py" "$NEWROOT/opt/aadongle/ota-server/"
fi

# Config web server
if [ -d "$RADXA_DIR/web" ]; then
    cp -r "$RADXA_DIR/web/"* "$NEWROOT/opt/aadongle/web/"
fi

ok "All AADongle files installed"

# ---------------------------------------------------------------------------
# Step 6: System configuration
# ---------------------------------------------------------------------------
step "[6/9] Configuring system..."

# Hostname
echo "aadongle" > "$NEWROOT/etc/hostname"
cat > "$NEWROOT/etc/hosts" << 'EOF'
127.0.0.1 localhost aadongle
::1       localhost aadongle
EOF

# Network — virtual AP interface (ap0) gets static IP via systemd service
# wlan0 stays free for NetworkManager (dev SSH access)
# The ap0-setup.service creates the virtual interface and assigns the IP
# See: radxa/scripts/setup.sh for the service definition

# Loopback
cat > "$NEWROOT/etc/network/interfaces" << 'EOF'
auto lo
iface lo inet loopback
source /etc/network/interfaces.d/*
EOF

# hostapd
mkdir -p "$NEWROOT/etc/hostapd"
cp "$CONFIG_DIR/hostapd.conf" "$NEWROOT/etc/hostapd/hostapd.conf"
chmod 640 "$NEWROOT/etc/hostapd/hostapd.conf"
mkdir -p "$NEWROOT/etc/default"
echo 'DAEMON_CONF="/etc/hostapd/hostapd.conf"' > "$NEWROOT/etc/default/hostapd"

# dnsmasq
mkdir -p "$NEWROOT/etc/dnsmasq.d"
cp "$CONFIG_DIR/dnsmasq.conf" "$NEWROOT/etc/dnsmasq.d/aadongle.conf"

# Bluetooth
mkdir -p "$NEWROOT/etc/bluetooth"
cp "$CONFIG_DIR/bluetooth.conf" "$NEWROOT/etc/bluetooth/main.conf"

# sysctl
mkdir -p "$NEWROOT/etc/sysctl.d"
cat > "$NEWROOT/etc/sysctl.d/99-aadongle.conf" << 'EOF'
net.ipv4.ip_forward=1
kernel.printk = 3 4 1 3
vm.swappiness = 0
vm.dirty_ratio = 20
vm.dirty_background_ratio = 5
EOF

# Kernel modules to load at boot
cat > "$NEWROOT/etc/modules" << 'EOF'
v4l2_common
videobuf2_common
EOF

# Journald — volatile (RAM only)
mkdir -p "$NEWROOT/etc/systemd/journald.conf.d"
cp "$CONFIG_DIR/journald-volatile.conf" "$NEWROOT/etc/systemd/journald.conf.d/volatile.conf"

# fstab — auto-detect root device, tmpfs mounts
ROOT_DEV=$(findmnt -n -o SOURCE / 2>/dev/null || echo "/dev/mmcblk1p3")
cat > "$NEWROOT/etc/fstab" << EOF
# <device>    <mount>     <type>  <options>                     <dump> <pass>
${ROOT_DEV}     /         ext4    noatime,errors=remount-ro     0      1
tmpfs           /tmp      tmpfs   nosuid,nodev,size=64M         0      0
tmpfs           /var/log  tmpfs   nosuid,nodev,size=32M         0      0
tmpfs           /var/tmp  tmpfs   nosuid,nodev,size=16M         0      0
tmpfs           /run      tmpfs   nosuid,nodev,mode=0755,size=32M 0    0
EOF

# Create /data mount point
mkdir -p "$NEWROOT/data"

# Default config file
mkdir -p "$NEWROOT/opt/aadongle/config"
cat > "$NEWROOT/opt/aadongle/config/defaults.json" << 'DEFAULTS'
{
    "wifi_ssid": "AADongle",
    "wifi_password": "AADongle5GHz!",
    "wifi_channel": 36,
    "display_mode": "full_aa",
    "ir_led_auto": true,
    "ir_led_brightness": 100,
    "version": "1.0.0"
}
DEFAULTS

# --- User setup (radxa user with sudo) ---
chroot "$NEWROOT" useradd -m -s /bin/bash -G sudo radxa 2>/dev/null || true
# Copy password hash from running system so same password works
RADXA_SHADOW=$(grep '^radxa:' /etc/shadow 2>/dev/null || true)
if [ -n "$RADXA_SHADOW" ]; then
    sed -i "s|^radxa:.*|$RADXA_SHADOW|" "$NEWROOT/etc/shadow"
    ok "radxa user created (password copied from running system)"
else
    echo "radxa:radxa" | chroot "$NEWROOT" chpasswd
    ok "radxa user created (default password: radxa)"
fi

# Allow sudo without password (same as stock image)
echo "radxa ALL=(ALL) NOPASSWD: ALL" > "$NEWROOT/etc/sudoers.d/radxa"
chmod 440 "$NEWROOT/etc/sudoers.d/radxa"

# --- SSH setup ---
# Copy host keys so SSH fingerprint doesn't change after deploy
if [ -d /etc/ssh ]; then
    cp /etc/ssh/ssh_host_* "$NEWROOT/etc/ssh/" 2>/dev/null || true
    ok "SSH host keys copied from running system"
fi

# Copy authorized_keys for radxa user
if [ -f /home/radxa/.ssh/authorized_keys ]; then
    mkdir -p "$NEWROOT/home/radxa/.ssh"
    cp /home/radxa/.ssh/authorized_keys "$NEWROOT/home/radxa/.ssh/"
    chroot "$NEWROOT" chown -R radxa:radxa /home/radxa/.ssh
    chmod 700 "$NEWROOT/home/radxa/.ssh"
    chmod 600 "$NEWROOT/home/radxa/.ssh/authorized_keys"
    ok "SSH authorized_keys copied"
fi

# --- NetworkManager config ---
# Tell NM to leave ap0 alone (hostapd manages it), keep wlan0 managed for dev SSH
mkdir -p "$NEWROOT/etc/NetworkManager/conf.d"
cat > "$NEWROOT/etc/NetworkManager/conf.d/aadongle-unmanaged.conf" <<'NM_EOF'
[keyfile]
unmanaged-devices=interface-name:ap0
NM_EOF

# Copy WiFi connection profiles so NM can auto-connect on boot
if [ -d /etc/NetworkManager/system-connections ]; then
    mkdir -p "$NEWROOT/etc/NetworkManager/system-connections"
    cp /etc/NetworkManager/system-connections/* \
       "$NEWROOT/etc/NetworkManager/system-connections/" 2>/dev/null || true
    ok "NetworkManager WiFi profiles copied (dev SSH will work after deploy)"
fi

# --- ap0-setup.service (creates virtual AP interface) ---
cat > "$NEWROOT/etc/systemd/system/ap0-setup.service" <<'UNIT_EOF'
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

# --- hostapd drop-in: wait for ap0 ---
mkdir -p "$NEWROOT/etc/systemd/system/hostapd.service.d"
cat > "$NEWROOT/etc/systemd/system/hostapd.service.d/wait-for-ap0.conf" <<'DROP_EOF'
[Unit]
Requires=ap0-setup.service
After=ap0-setup.service
DROP_EOF

# --- dnsmasq drop-in: wait for ap0 ---
mkdir -p "$NEWROOT/etc/systemd/system/dnsmasq.service.d"
cat > "$NEWROOT/etc/systemd/system/dnsmasq.service.d/wait-for-ap0.conf" <<'DROP_EOF'
[Unit]
Requires=ap0-setup.service
After=ap0-setup.service
DROP_EOF

ok "System configured"

# ---------------------------------------------------------------------------
# Step 7: Systemd services
# ---------------------------------------------------------------------------
step "[7/9] Installing systemd services..."

SYSTEMD_DIR="$NEWROOT/etc/systemd/system"
mkdir -p "$SYSTEMD_DIR"

# Copy all service files
for f in "$CONFIG_DIR/systemd/"*.service "$CONFIG_DIR/systemd/"*.target; do
    [ -f "$f" ] && cp "$f" "$SYSTEMD_DIR/"
done

# Enable services in chroot
chroot "$NEWROOT" bash -c "
    # Critical system services
    systemctl enable ssh 2>/dev/null || true
    systemctl enable NetworkManager 2>/dev/null || true
    systemctl enable ap0-setup 2>/dev/null || true
    systemctl enable hostapd 2>/dev/null || true
    systemctl enable dnsmasq 2>/dev/null || true
    systemctl enable bluetooth 2>/dev/null || true
    systemctl enable avahi-daemon 2>/dev/null || true
    # AADongle services
    systemctl enable aadongle.target 2>/dev/null || true
    systemctl enable aa-bridge 2>/dev/null || true
    systemctl enable aa-proxy 2>/dev/null || true
    systemctl enable carplay 2>/dev/null || true
    systemctl enable baby-monitor 2>/dev/null || true
    systemctl enable baby-monitor-api 2>/dev/null || true
    systemctl enable bt-agent 2>/dev/null || true
    systemctl enable ir-leds 2>/dev/null || true
    systemctl enable ota-server 2>/dev/null || true
    systemctl enable power-manager 2>/dev/null || true
    systemctl enable config-server 2>/dev/null || true
    systemctl set-default multi-user.target
"

# Mask unnecessary services
chroot "$NEWROOT" bash -c "
    systemctl mask apt-daily.timer 2>/dev/null || true
    systemctl mask apt-daily-upgrade.timer 2>/dev/null || true
    systemctl mask systemd-journal-flush.service 2>/dev/null || true
    systemctl mask emergency.service 2>/dev/null || true
    systemctl mask rescue.service 2>/dev/null || true
    systemctl mask swap.target 2>/dev/null || true
"

# Disable unused generators
GENDIR="$NEWROOT/etc/systemd/system-generators"
mkdir -p "$GENDIR"
for gen in systemd-debug-generator systemd-gpt-auto-generator systemd-sysv-generator; do
    ln -sf /dev/null "$GENDIR/$gen"
done

ok "Services installed and enabled"

# ---------------------------------------------------------------------------
# Step 8: Read-only root + overlayfs
# ---------------------------------------------------------------------------
step "[8/9] Setting up read-only root with overlayfs..."

# Install initramfs overlay scripts
mkdir -p "$NEWROOT/etc/initramfs-tools/scripts/init-bottom"
mkdir -p "$NEWROOT/etc/initramfs-tools/hooks"

cp "$CONFIG_DIR/overlayfs/overlay-init" \
   "$NEWROOT/etc/initramfs-tools/scripts/init-bottom/overlay-root"
chmod 755 "$NEWROOT/etc/initramfs-tools/scripts/init-bottom/overlay-root"

cp "$CONFIG_DIR/overlayfs/overlay-hook" \
   "$NEWROOT/etc/initramfs-tools/hooks/overlay-root"
chmod 755 "$NEWROOT/etc/initramfs-tools/hooks/overlay-root"

# Rebuild initramfs inside chroot
chroot "$NEWROOT" update-initramfs -u -k "$KERNEL_VER" 2>/dev/null || \
    warn "Could not rebuild initramfs in chroot — will need rebuild on first boot"

# Kernel cmdline (update /boot/uEnv.txt on host — boot partition shared)
UENV="/boot/uEnv.txt"
if [ -f "$UENV" ]; then
    EXTRA="quiet loglevel=0 fsck.mode=skip systemd.show_status=false"
    if grep -q "^extraargs=" "$UENV"; then
        current=$(grep "^extraargs=" "$UENV" | cut -d= -f2-)
        for arg in $EXTRA; do
            echo "$current" | grep -q "$arg" || current="$current $arg"
        done
        sed -i "s|^extraargs=.*|extraargs=$current|" "$UENV"
    else
        echo "extraargs=$EXTRA" >> "$UENV"
    fi
    ok "Kernel cmdline updated"
fi

ok "Read-only root configured"

# ---------------------------------------------------------------------------
# Step 9: Deploy minimal rootfs
# ---------------------------------------------------------------------------
step "[9/9] Deploying minimal rootfs..."

# Show size comparison
echo ""
echo "  Current rootfs: $(du -sh / --exclude=/proc --exclude=/sys --exclude=/dev --exclude=/mnt 2>/dev/null | cut -f1)"
echo "  New rootfs:     $(du -sh "$NEWROOT" | cut -f1)"
echo "  Packages:       $(chroot "$NEWROOT" dpkg --list | grep '^ii' | wc -l)"
echo ""

# Safety: back up current rootfs manifest
dpkg --list > /tmp/old-rootfs-packages.txt 2>/dev/null || true

if [ "${1:-}" = "--no-deploy" ]; then
    ok "Minimal rootfs built at $NEWROOT (--no-deploy: skipping deployment)"
    echo ""
    echo "To deploy manually later:"
    echo "  sudo bash $0 --deploy-only"
    echo ""
    # Unmount will happen via trap
    exit 0
fi

echo ""
echo -e "${YLW}WARNING: About to replace the current rootfs and force reboot.${RST}"
echo -e "${YLW}The system will rsync the new rootfs over / and immediately reboot.${RST}"
echo ""
echo "Press Ctrl-C within 10 seconds to abort..."
sleep 10

echo ""
step "Deploying via rsync..."

# Unmount chroot bind mounts before rsync (avoid copying /proc etc from chroot)
cleanup_mounts

# rsync the new rootfs over the live system
# Exclude: virtual filesystems, build area, boot partition, build tools, swap
rsync -aAX --delete "$NEWROOT/" / \
    --exclude=/proc \
    --exclude=/sys \
    --exclude=/dev \
    --exclude=/mnt \
    --exclude=/boot \
    --exclude=/data \
    --exclude=/tmp \
    --exclude=/swapfile \
    --exclude=/home/radxa/TinySightAI \
    --exclude=/root/.cargo \
    --exclude=/root/.rustup \
    2>&1 || true

ok "rsync complete"

# Force immediate reboot via kernel sysrq
# This bypasses userspace entirely — no reliance on replaced binaries
echo ""
echo "Syncing disks and force-rebooting via sysrq..."
sync
sleep 1
echo s > /proc/sysrq-trigger  # sync all filesystems
sleep 2
echo b > /proc/sysrq-trigger  # immediate reboot

# Should never reach here, but just in case
sleep 5
reboot -f 2>/dev/null || true

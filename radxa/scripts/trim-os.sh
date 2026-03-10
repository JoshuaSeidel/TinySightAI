#!/bin/bash
# =============================================================================
# trim-os.sh — Trim Radxa Debian to minimise RAM usage (~200 MB idle target)
#
# NOTE: avahi-daemon is intentionally NOT removed — it is required for
# CarPlay Bonjour / mDNS service advertisement (_airplay._tcp, _raop._tcp).
#
# Usage: sudo bash trim-os.sh
#        (also called automatically by setup.sh)
# =============================================================================
set -euo pipefail

# Color helpers (work standalone or when sourced from setup.sh)
if ! declare -f ok >/dev/null 2>&1; then
    RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; RST='\033[0m'
    ok()   { echo -e "  ${GRN}OK${RST}  $*"; }
    warn() { echo -e "  ${YLW}WARN${RST} $*"; }
    step() { echo -e "\n==> $*"; }
fi

echo ""
echo "=== Trimming Radxa Debian for AADongle (low-RAM headless target) ==="

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Run as root" >&2
    exit 1
fi

# =============================================================================
# 1. Disable services that waste RAM on a headless appliance
#    avahi-daemon intentionally absent — CarPlay needs it for mDNS.
# =============================================================================
step "[1/5] Disabling unused services..."

DISABLE_SERVICES=(
    ModemManager
    # NetworkManager — KEEP for dev SSH over WiFi (wlan0 STA mode)
    # wpa_supplicant — KEEP (required by NetworkManager for WiFi)
    cups
    cups-browsed
    triggerhappy
    rsyslog
    cron
    systemd-timesyncd
)

for svc in "${DISABLE_SERVICES[@]}"; do
    if systemctl is-enabled "$svc" 2>/dev/null | grep -q "enabled\|static"; then
        systemctl disable --now "$svc" 2>/dev/null || true
        ok "Disabled: $svc"
    else
        echo "  skip (not enabled): $svc"
    fi
done

# =============================================================================
# 2. Remove desktop / GUI packages if a desktop image was used
#    avahi-daemon intentionally absent from purge list.
# =============================================================================
step "[2/5] Removing desktop packages (if any)..."
apt-get remove --purge -y \
    xserver-xorg* lightdm* lxde* lxqt* \
    chromium* firefox* \
    libreoffice* \
    pulseaudio* pipewire* \
    2>/dev/null || true
ok "Desktop packages removed (or were already absent)"

# =============================================================================
# 3. Remove packages that are not needed on this appliance.
#    avahi-daemon and avahi-utils are explicitly kept.
#    cups is removed (printer daemon — irrelevant here).
# =============================================================================
step "[3/5] Removing unneeded packages..."
apt-get remove --purge -y \
    modemmanager \
    cups \
    man-db manpages \
    2>/dev/null || true
# NOTE: network-manager is NOT removed — needed for dev WiFi SSH access.
# It will be removed later by build-minimal-rootfs.sh for production.
ok "Unneeded packages removed"

# =============================================================================
# 4. Clean package cache to free storage
# =============================================================================
step "[4/5] Cleaning package cache..."
apt-get autoremove -y
apt-get clean
rm -rf /var/cache/apt/archives/* /tmp/*
ok "Package cache cleaned"

# =============================================================================
# 5. Kernel tunables — quieter console, reduced page-cache pressure
# =============================================================================
step "[5/5] Applying sysctl tunables..."

SYSCTL_FILE=/etc/sysctl.d/99-aadongle.conf

# Preserve any lines we may have already written (e.g. ip_forward from setup.sh)
if ! grep -q "kernel.printk" "$SYSCTL_FILE" 2>/dev/null; then
    cat >> "$SYSCTL_FILE" <<'SYSCTL_EOF'

# Reduce kernel console noise
kernel.printk = 3 4 1 3

# Reduce VM pressure on a 1 GB device
vm.swappiness = 10
vm.dirty_ratio = 20
vm.dirty_background_ratio = 5
SYSCTL_EOF
fi

sysctl --system >/dev/null 2>&1 || sysctl -p "$SYSCTL_FILE" 2>/dev/null || true
ok "sysctl tunables applied"

# =============================================================================
# 6. Boot speed — disable slow systemd generators & units
# =============================================================================
step "[6/7] Disabling slow boot components..."

MASK_BOOT=(
    apt-daily.timer
    apt-daily-upgrade.timer
    man-db.timer
    e2scrub_all.timer
    fstrim.timer
    logrotate.timer
    systemd-journal-flush.service
    emergency.service
    rescue.service
)

for svc in "${MASK_BOOT[@]}"; do
    systemctl mask "$svc" 2>/dev/null && ok "Masked: $svc" || true
done

# Disable systemd generators we don't need (saves ~0.5s)
GENERATOR_DIR=/etc/systemd/system-generators
mkdir -p "$GENERATOR_DIR"
for gen in systemd-debug-generator systemd-fstab-generator \
           systemd-gpt-auto-generator systemd-sysv-generator; do
    ln -sf /dev/null "$GENERATOR_DIR/$gen" 2>/dev/null || true
done
ok "Disabled unused systemd generators"

# =============================================================================
# 7. Disable swap entirely (1GB RAM is enough, swap kills SD cards)
# =============================================================================
step "[7/7] Disabling swap..."

swapoff -a 2>/dev/null || true
# Remove any swap entries from fstab
sed -i '/\sswap\s/d' /etc/fstab 2>/dev/null || true
systemctl mask swap.target 2>/dev/null || true
# Set swappiness to 0 (belt and suspenders)
echo "vm.swappiness = 0" >> "$SYSCTL_FILE" 2>/dev/null || true
ok "Swap disabled (protects SD card lifespan)"

echo ""
echo "=== Trim complete ==="
echo "Current memory usage:"
free -h
echo ""
echo "Next: run make-readonly.sh to enable read-only root with tmpfs overlay."
echo "Reboot to see full effect of service disables."

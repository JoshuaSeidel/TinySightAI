#!/bin/bash
# make-readonly.sh — Convert Radxa rootfs to read-only with tmpfs overlay
#
# Run ONCE after setup.sh. After reboot, the root filesystem is immutable.
# All runtime writes go to RAM (tmpfs). Hard power cut = zero corruption.
#
# To make persistent changes later:
#   1. Boot with "disable_overlayfs" on kernel cmdline (hold U-Boot key)
#   2. Or: mount -o remount,rw /overlay/lower  (temporary rw access)
#   3. Make changes, then reboot
#
# Usage: sudo ./make-readonly.sh

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Must run as root"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CONFIG_DIR="$PROJECT_ROOT/radxa/config"

echo "=== AADongle Read-Only Root Setup ==="
echo ""

# ── 1. Install initramfs overlay scripts ──────────────────────────────
echo "[1/7] Installing initramfs overlay scripts..."

cp "$CONFIG_DIR/overlayfs/overlay-init" \
   /etc/initramfs-tools/scripts/init-bottom/overlay-root
chmod 755 /etc/initramfs-tools/scripts/init-bottom/overlay-root

cp "$CONFIG_DIR/overlayfs/overlay-hook" \
   /etc/initramfs-tools/hooks/overlay-root
chmod 755 /etc/initramfs-tools/hooks/overlay-root

echo "  Installed overlay-root init script and hook"

# ── 2. Set root filesystem to read-only in fstab ─────────────────────
echo "[2/7] Setting root mount to read-only in /etc/fstab..."

# Replace 'defaults' or 'errors=remount-ro' with 'ro,noatime' for root mount
if grep -q ' / ' /etc/fstab; then
    sed -i 's|\( / \s*ext4\s*\)[^ ]*|\1ro,noatime,errors=remount-ro|' /etc/fstab
    echo "  Updated /etc/fstab root entry to ro,noatime"
else
    echo "  WARNING: Could not find root entry in /etc/fstab — check manually"
fi

# ── 3. Add tmpfs mounts for volatile directories ─────────────────────
echo "[3/7] Adding tmpfs mounts to /etc/fstab..."

# Only add if not already present
for entry in \
    "tmpfs /tmp tmpfs nosuid,nodev,size=64M 0 0" \
    "tmpfs /var/log tmpfs nosuid,nodev,size=32M 0 0" \
    "tmpfs /var/tmp tmpfs nosuid,nodev,size=16M 0 0" \
    "tmpfs /var/spool tmpfs nosuid,nodev,size=8M 0 0" \
    "tmpfs /run tmpfs nosuid,nodev,mode=0755,size=32M 0 0"
do
    mount_point=$(echo "$entry" | awk '{print $2}')
    if ! grep -q " $mount_point " /etc/fstab; then
        echo "$entry" >> /etc/fstab
        echo "  Added tmpfs mount: $mount_point"
    fi
done

# ── 4. Configure journald for volatile storage ───────────────────────
echo "[4/7] Configuring journald for volatile (RAM) storage..."

mkdir -p /etc/systemd/journald.conf.d
cp "$CONFIG_DIR/journald-volatile.conf" \
   /etc/systemd/journald.conf.d/volatile.conf 2>/dev/null || \
cat > /etc/systemd/journald.conf.d/volatile.conf << 'JEOF'
[Journal]
Storage=volatile
RuntimeMaxUse=16M
RuntimeMaxFileSize=4M
MaxLevelStore=warning
JEOF
echo "  Journald set to volatile (RAM only, 16MB max)"

# ── 5. Kernel cmdline — fast boot parameters ─────────────────────────
echo "[5/7] Optimizing kernel command line..."

UENV="/boot/uEnv.txt"
if [ -f "$UENV" ]; then
    # Add boot parameters if not already present
    EXTRA_ARGS="quiet loglevel=0 fsck.mode=skip systemd.show_status=false"
    if grep -q "^extraargs=" "$UENV"; then
        # Append to existing extraargs
        current=$(grep "^extraargs=" "$UENV" | cut -d= -f2-)
        for arg in $EXTRA_ARGS; do
            if ! echo "$current" | grep -q "$arg"; then
                current="$current $arg"
            fi
        done
        sed -i "s|^extraargs=.*|extraargs=$current|" "$UENV"
    else
        echo "extraargs=$EXTRA_ARGS" >> "$UENV"
    fi
    echo "  Updated $UENV with fast boot parameters"
else
    echo "  WARNING: $UENV not found — add to kernel cmdline manually:"
    echo "    quiet loglevel=0 fsck.mode=skip systemd.show_status=false"
fi

# ── 6. Disable unnecessary systemd generators and targets ────────────
echo "[6/7] Disabling unnecessary systemd components..."

# Mask services that slow boot on embedded
MASK_SERVICES=(
    apt-daily.timer
    apt-daily-upgrade.timer
    man-db.timer
    e2scrub_all.timer
    fstrim.timer
    logrotate.timer
    systemd-fsck-root.service
    systemd-fsck@.service
    systemd-journal-flush.service
)

for svc in "${MASK_SERVICES[@]}"; do
    systemctl mask "$svc" 2>/dev/null && \
        echo "  Masked: $svc" || true
done

# Disable emergency/rescue shell (headless device — no console)
systemctl mask emergency.service 2>/dev/null || true
systemctl mask rescue.service 2>/dev/null || true

# ── 7. Rebuild initramfs ─────────────────────────────────────────────
echo "[7/7] Rebuilding initramfs with overlay support..."

update-initramfs -u
echo "  Initramfs rebuilt"

# ── Done ──────────────────────────────────────────────────────────────
echo ""
echo "=== Read-Only Root Setup Complete ==="
echo ""
echo "After reboot:"
echo "  - Root filesystem is READ-ONLY (immutable)"
echo "  - All writes go to tmpfs (RAM) — lost on power cut (by design)"
echo "  - Hard power cut cannot corrupt the filesystem"
echo "  - No fsck on boot = faster startup"
echo ""
echo "To make persistent changes:"
echo "  mount -o remount,rw /overlay/lower"
echo "  # make your changes..."
echo "  mount -o remount,ro /overlay/lower"
echo ""
echo "To bypass overlay (maintenance mode):"
echo "  Add 'disable_overlayfs' to kernel cmdline in U-Boot"
echo ""
echo "Reboot now to activate: sudo reboot"

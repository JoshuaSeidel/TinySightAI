#!/bin/bash
# =============================================================================
# deploy-rootfs.sh — Extract staged rootfs tarball over live root
#
# Called by deploy-rootfs.service on boot when /boot/rootfs-pending.tar.gz exists.
# Extracts the new rootfs over /, excluding /boot (separate partition) and
# virtual filesystems. After successful extraction, renames the tarball and
# reboots into the new rootfs.
#
# Safe: if extraction fails (power loss), tarball stays → auto-retries next boot.
# Recovery: mount SD on PC, run /boot/recover.sh
# =============================================================================
set -euo pipefail

TARBALL="/boot/rootfs-pending.tar.gz"

echo "deploy-rootfs: deploying staged rootfs from $TARBALL..."
echo "deploy-rootfs: tarball size: $(du -h "$TARBALL" | cut -f1)"

# Extract tarball over current root
# --keep-directory-symlink: preserve merged-usr symlinks (/bin -> /usr/bin etc)
# Exclude: /boot (separate partition), virtual filesystems, runtime dirs
tar xzf "$TARBALL" -C / \
    --keep-directory-symlink \
    --exclude='./boot' \
    --exclude='./proc' \
    --exclude='./sys' \
    --exclude='./dev' \
    --exclude='./run'

echo "deploy-rootfs: extraction complete"

# Success — rename tarball so we don't re-deploy on next boot
mv "$TARBALL" /boot/rootfs-deployed.tar.gz
echo "deploy-rootfs: tarball renamed to rootfs-deployed.tar.gz"

# Sync filesystem buffers
sync

echo "deploy-rootfs: rebooting into new rootfs..."
# Force reboot — avoids shutdown sequence issues after rootfs replacement
systemctl reboot --force

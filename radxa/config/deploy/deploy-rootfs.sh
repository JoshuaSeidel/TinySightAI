#!/bin/bash
# =============================================================================
# deploy-rootfs.sh — Extract staged rootfs tarball over live root
#
# Called by deploy-rootfs.service on boot when /boot/rootfs-pending.tar.gz exists.
# Extracts the new rootfs over /, excluding /boot (separate partition) and
# virtual filesystems. Handles merged-usr transition (new rootfs uses
# /bin -> usr/bin symlinks, stock system has real directories).
#
# Safe: if extraction fails (power loss), tarball stays → auto-retries next boot.
# Recovery: mount SD on PC, run /boot/recover.sh
# =============================================================================
set -uo pipefail

TARBALL="/boot/rootfs-pending.tar.gz"

echo "deploy-rootfs: deploying staged rootfs from $TARBALL..."
echo "deploy-rootfs: tarball size: $(du -h "$TARBALL" | cut -f1)"

# Extract tarball over current root
# Merged-usr tarballs contain /bin, /sbin, /lib as symlinks → usr/*
# These fail if stock system has real directories — that's expected.
# All actual files are under /usr/* and extract fine.
tar xzf "$TARBALL" -C / \
    --keep-directory-symlink \
    --exclude='./boot' \
    --exclude='./proc' \
    --exclude='./sys' \
    --exclude='./dev' \
    --exclude='./run' \
    2>/tmp/deploy-tar-errors.log || true

# Check for real errors (ignore merged-usr symlink warnings)
if grep -v "Cannot create symlink" /tmp/deploy-tar-errors.log | grep -q "^tar:"; then
    echo "deploy-rootfs: FATAL: tar had unexpected errors:"
    cat /tmp/deploy-tar-errors.log
    exit 1
fi

echo "deploy-rootfs: extraction complete"

# Handle merged-usr transition: convert /bin and /sbin to symlinks
# /lib left as directory — the dynamic linker lives there and is needed
# to exec mv/ln. System works because /usr/lib has new libs and ld.so.cache
# points there.
for d in bin sbin; do
    if [ -d "/$d" ] && [ ! -L "/$d" ]; then
        echo "deploy-rootfs: converting /$d -> usr/$d"
        mv "/$d" "/${d}.old"
        ln -s "usr/$d" "/$d"
        rm -rf "/${d}.old" &
    fi
done

if [ -d "/lib" ] && [ ! -L "/lib" ]; then
    echo "deploy-rootfs: /lib kept as directory (dynamic linker needed for this boot)"
fi

# Wait for background cleanup
wait

# Success — rename tarball so we don't re-deploy on next boot
mv "$TARBALL" /boot/rootfs-deployed.tar.gz
echo "deploy-rootfs: tarball renamed to rootfs-deployed.tar.gz"

# Sync filesystem buffers
sync

echo "deploy-rootfs: rebooting into new rootfs..."
# Force reboot — avoids shutdown sequence issues after rootfs replacement
systemctl reboot --force

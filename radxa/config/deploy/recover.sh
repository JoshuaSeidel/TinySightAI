#!/bin/bash
# =============================================================================
# recover.sh — Restore rootfs from tarball on /boot
#
# Run this from a Linux PC with the SD card inserted:
#   sudo bash /media/$USER/boot/recover.sh /dev/sdX3
#
# The script formats the root partition and extracts the rootfs tarball.
# No reflash needed.
# =============================================================================
set -euo pipefail

ROOT_PART="${1:?Usage: $0 /dev/sdXN  (root partition of the SD card)}"

# Sanity check
if [ ! -b "$ROOT_PART" ]; then
    echo "ERROR: $ROOT_PART is not a block device" >&2
    exit 1
fi

# Find tarball (prefer deployed, fall back to pending)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARBALL=""
for candidate in "$SCRIPT_DIR/rootfs-deployed.tar.gz" "$SCRIPT_DIR/rootfs-pending.tar.gz"; do
    if [ -f "$candidate" ]; then
        TARBALL="$candidate"
        break
    fi
done

if [ -z "$TARBALL" ]; then
    echo "ERROR: No rootfs tarball found on /boot" >&2
    echo "  Looked for: rootfs-deployed.tar.gz, rootfs-pending.tar.gz" >&2
    exit 1
fi

echo "Tarball:    $(basename "$TARBALL") ($(du -h "$TARBALL" | cut -f1))"
echo "Target:     $ROOT_PART"
echo ""
echo "This will FORMAT $ROOT_PART and extract the rootfs."
echo "Press Ctrl-C within 5 seconds to abort..."
sleep 5

echo ""
echo "Formatting $ROOT_PART as ext4..."
mkfs.ext4 -F "$ROOT_PART"

MNT=$(mktemp -d)
mount "$ROOT_PART" "$MNT"

echo "Extracting rootfs..."
tar xzf "$TARBALL" -C "$MNT"

umount "$MNT"
rmdir "$MNT"

echo ""
echo "Done. Insert the SD card into the device and boot."

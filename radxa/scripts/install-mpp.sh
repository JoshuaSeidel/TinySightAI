#!/bin/bash
# Build and install ffmpeg-rockchip for hardware H.264 encode/decode on RK3566
set -euo pipefail

echo "=== Building ffmpeg-rockchip ==="

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Run as root"
    exit 1
fi

BUILD_DIR="/tmp/ffmpeg-build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "[1/4] Installing build dependencies..."
apt-get install -y \
    build-essential cmake pkg-config nasm yasm \
    librockchip-mpp-dev librockchip-mpp1 \
    librga-dev librga2 \
    libdrm-dev \
    libx264-dev libx265-dev \
    libass-dev libfreetype-dev \
    libmp3lame-dev libvorbis-dev \
    texinfo zlib1g-dev

echo "[2/4] Cloning ffmpeg-rockchip..."
if [ -d "ffmpeg-rockchip" ]; then
    cd ffmpeg-rockchip && git pull
else
    git clone https://github.com/nyanmisaka/ffmpeg-rockchip.git
    cd ffmpeg-rockchip
fi

echo "[3/4] Configuring ffmpeg..."
./configure \
    --enable-rkmpp \
    --enable-rkrga \
    --enable-version3 \
    --enable-libdrm \
    --enable-gpl \
    --enable-libx264 \
    --enable-nonfree \
    --disable-doc \
    --disable-debug \
    --enable-small

echo "[4/4] Building ffmpeg (this takes a while on RK3566)..."
make -j$(nproc)
make install
ldconfig

echo ""
echo "=== ffmpeg-rockchip installed ==="
echo "Verifying hardware codecs:"
echo ""
echo "Decoders:"
ffmpeg -decoders 2>/dev/null | grep rkmpp || echo "  WARNING: No rkmpp decoders found"
echo ""
echo "Encoders:"
ffmpeg -encoders 2>/dev/null | grep rkmpp || echo "  WARNING: No rkmpp encoders found"
echo ""

# Clean up build dir
cd /
rm -rf "$BUILD_DIR"
echo "Build directory cleaned."

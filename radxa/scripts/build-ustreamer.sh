#!/bin/bash
# Build µStreamer for MJPEG camera streaming (phone web UI)
set -euo pipefail

INSTALL_DIR="/opt/aadongle/bin"

echo "=== Building µStreamer ==="

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Run as root"
    exit 1
fi

apt-get install -y build-essential libevent-dev libjpeg62-turbo-dev libbsd-dev

BUILD_DIR="/tmp/ustreamer-build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "ustreamer" ]; then
    cd ustreamer && git pull
else
    git clone https://github.com/pikvm/ustreamer.git
    cd ustreamer
fi

make -j$(nproc)

mkdir -p "$INSTALL_DIR"
cp ustreamer "$INSTALL_DIR/"

echo ""
echo "=== µStreamer installed to $INSTALL_DIR/ustreamer ==="
echo "Test: $INSTALL_DIR/ustreamer --device /dev/video0 --host 0.0.0.0 --port 8082"

cd /
rm -rf "$BUILD_DIR"

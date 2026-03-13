#!/bin/bash
# power-manager.sh — Watchdog + Power Monitor (fast boot version)
#
# NO LONGER starts services — systemd handles that natively and in parallel.
# This script only:
#   1. Sets CPU governor to performance (instant)
#   2. Exports GPIO for USB power monitoring
#   3. Runs watchdog loop (ping compositor)
#   4. Monitors USB power loss → halt

set -euo pipefail

LOG_TAG="power-manager"
COMPOSITOR_CTRL_HOST="127.0.0.1"
COMPOSITOR_CTRL_PORT=5290
WATCHDOG_INTERVAL=10
WATCHDOG_FAIL_LIMIT=3

# TODO: Verify correct USB power sense GPIO on Cubie A7Z 40-pin header
USB_POWER_GPIO_NUM=39
USB_POWER_GPIO="/sys/class/gpio/gpio${USB_POWER_GPIO_NUM}/value"
USB_POWER_EXPORT="/sys/class/gpio/export"
USB_POWER_DIRECTION="/sys/class/gpio/gpio${USB_POWER_GPIO_NUM}/direction"

# ── Logging ───────────────────────────────────────────────────────────
log_info()  { echo "[$LOG_TAG] $*"; }
log_warn()  { echo "[$LOG_TAG] WARN: $*" >&2; }
log_error() { echo "[$LOG_TAG] ERROR: $*" >&2; }

# ── GPIO ──────────────────────────────────────────────────────────────
gpio_export() {
    if [ ! -e "$USB_POWER_DIRECTION" ]; then
        echo "$USB_POWER_GPIO_NUM" > "$USB_POWER_EXPORT" 2>/dev/null || true
        echo "in" > "$USB_POWER_DIRECTION" 2>/dev/null || true
    fi
}

gpio_read_usb_power() {
    local val
    val=$(cat "$USB_POWER_GPIO" 2>/dev/null || echo "1")
    [ "$val" = "1" ]
}

# ── Init (runs once, fast — no sleeps) ───────────────────────────────
init() {
    log_info "Starting (PID $$)"

    # CPU performance governor — instant
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$gov" 2>/dev/null || true
    done
    log_info "CPU governor: performance"

    # Export GPIO for power monitoring
    gpio_export

    # Check camera presence
    [ -e /dev/video0 ] && log_info "Camera: /dev/video0 OK" || \
        log_warn "Camera: /dev/video0 not found"
}

# ── Compositor watchdog ──────────────────────────────────────────────
ping_compositor() {
    local resp
    resp=$(echo "STATUS" | timeout 3 nc -q1 "$COMPOSITOR_CTRL_HOST" "$COMPOSITOR_CTRL_PORT" 2>/dev/null || true)
    [ -n "$resp" ] && echo "$resp" | grep -q '"mode"'
}

# ── USB power monitor ────────────────────────────────────────────────
monitor_usb_power() {
    local was_present=1
    while true; do
        sleep 2
        if gpio_read_usb_power; then
            was_present=1
        elif [ "$was_present" -eq 1 ]; then
            log_warn "USB power lost — halting"
            # Read-only root: no sync needed, just halt
            systemctl poweroff --no-block || halt
            exit 0
        fi
    done
}

# ── Signal handlers ──────────────────────────────────────────────────
trap 'log_info "SIGTERM — exiting"; exit 0' SIGTERM
trap 'log_info "SIGINT — exiting"; exit 0' SIGINT

# ── Main ─────────────────────────────────────────────────────────────
init

# USB power monitor in background
monitor_usb_power &

# Watchdog in foreground (keeps systemd service alive)
log_info "Watchdog active"
fail_count=0
while true; do
    sleep "$WATCHDOG_INTERVAL"
    if ping_compositor; then
        fail_count=0
    else
        fail_count=$((fail_count + 1))
        log_warn "Compositor ping failed ($fail_count/$WATCHDOG_FAIL_LIMIT)"
        if [ "$fail_count" -ge "$WATCHDOG_FAIL_LIMIT" ]; then
            log_error "Compositor unresponsive — restarting"
            systemctl restart aa-bridge 2>/dev/null || true
            fail_count=0
        fi
    fi
done

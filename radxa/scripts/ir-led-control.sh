#!/bin/bash
# ir-led-control.sh — IR LED Auto-Control for Night Vision (LEGACY)
#
# NOTE: The compositor (ir_led.c) now handles auto-brightness detection
# directly from camera frames and toggles GPIO in-process. This script
# is kept as a standalone fallback for testing or use without the compositor.
#
# Controls 850nm IR LEDs wired to a Radxa Cubie A7Z GPIO pin.
# Four operating modes:
#
#   auto  — check camera brightness every 30 s, enable IR if dark
#   time  — ON from IR_TIME_ON_HOUR to IR_TIME_OFF_HOUR (defaults: 18:00–06:00)
#   on    — always on
#   off   — always off
#
# Mode selection (highest priority first):
#   1. /tmp/ir-mode file ("on", "off", "auto", "time") — manual override
#   2. IR_DEFAULT_MODE environment variable
#   3. Hard-coded default: "auto"
#
# GPIO:
#   Write 1 to GPIO sysfs to turn IR LEDs on, 0 to turn off.
#   Pin number: set IR_GPIO_NUM or edit IR_GPIO_NUM below.
#   For Radxa Cubie A7Z GPIO header layout see:
#     https://docs.radxa.com/en/cubie/a7z/hardware-use/pin-gpio
#
# Usage:
#   sudo ./ir-led-control.sh
#   Or run via systemd service (ir-leds.service).

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration (override via environment or edit here)
# ---------------------------------------------------------------------------

# GPIO pin number — Radxa Cubie A7Z (Allwinner A733) 40-pin header
# Allwinner GPIO formula: P{letter}{num} = num + 32 * letter_index
# PB7 = 7 + 32*1 = 39 (TODO: verify correct pin on actual hardware)
IR_GPIO_NUM="${IR_GPIO_NUM:-39}"

IR_GPIO_BASE="/sys/class/gpio"
IR_GPIO_DIR="${IR_GPIO_BASE}/gpio${IR_GPIO_NUM}"
IR_GPIO_VALUE="${IR_GPIO_DIR}/value"
IR_GPIO_EXPORT="${IR_GPIO_BASE}/export"
IR_GPIO_UNEXPORT="${IR_GPIO_BASE}/unexport"
IR_GPIO_DIRECTION="${IR_GPIO_DIR}/direction"

# Manual override file (written by baby-monitor API or compositor)
IR_MODE_FILE="/tmp/ir-mode"

# Default mode if no override file exists
IR_DEFAULT_MODE="${IR_DEFAULT_MODE:-auto}"

# Time-based mode hours (24-hour clock)
IR_TIME_ON_HOUR="${IR_TIME_ON_HOUR:-18}"   # 6 PM
IR_TIME_OFF_HOUR="${IR_TIME_OFF_HOUR:-6}"  # 6 AM

# Auto-mode: brightness threshold (0-255 scale, from v4l2 metadata)
# IR turns ON when brightness < threshold
IR_BRIGHTNESS_THRESHOLD="${IR_BRIGHTNESS_THRESHOLD:-40}"

# Auto-mode check interval (seconds)
IR_AUTO_CHECK_INTERVAL=30

# Camera device for brightness reading
CAMERA_DEVICE="${CAMERA_DEVICE:-/dev/video0}"

LOG_TAG="ir-led-control"

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

log() {
    local level="$1"
    shift
    echo "$(date '+%Y-%m-%d %H:%M:%S') [$LOG_TAG] [$level] $*"
}

log_info()  { log "INFO " "$@"; }
log_warn()  { log "WARN " "$@"; }
log_error() { log "ERROR" "$@"; }

# ---------------------------------------------------------------------------
# GPIO control
# ---------------------------------------------------------------------------

gpio_setup() {
    if [ "$IR_GPIO_NUM" = "XXX" ]; then
        log_warn "IR GPIO not configured (placeholder XXX) — running in dry-run mode"
        return 0
    fi

    if [ ! -e "$IR_GPIO_DIR" ]; then
        log_info "Exporting GPIO${IR_GPIO_NUM}"
        echo "$IR_GPIO_NUM" > "$IR_GPIO_EXPORT" 2>/dev/null || {
            log_error "Failed to export GPIO${IR_GPIO_NUM}"
            return 1
        }
        sleep 0.1
    fi

    echo "out" > "$IR_GPIO_DIRECTION" 2>/dev/null || {
        log_error "Failed to set GPIO${IR_GPIO_NUM} direction to output"
        return 1
    }

    log_info "GPIO${IR_GPIO_NUM} configured as output"
    return 0
}

gpio_write() {
    local val="$1"   # 0 or 1

    if [ "$IR_GPIO_NUM" = "XXX" ]; then
        log_info "[dry-run] GPIO write: $val"
        return 0
    fi

    echo "$val" > "$IR_GPIO_VALUE" 2>/dev/null || {
        log_warn "GPIO write failed for value $val"
        return 1
    }
}

# Track current LED state to avoid redundant writes
_IR_STATE="unknown"

ir_on() {
    if [ "$_IR_STATE" != "on" ]; then
        log_info "IR LEDs: ON"
        gpio_write 1
        _IR_STATE="on"
    fi
}

ir_off() {
    if [ "$_IR_STATE" != "off" ]; then
        log_info "IR LEDs: OFF"
        gpio_write 0
        _IR_STATE="off"
    fi
}

# ---------------------------------------------------------------------------
# Read mode from override file or use default
# ---------------------------------------------------------------------------

get_current_mode() {
    if [ -f "$IR_MODE_FILE" ]; then
        local mode
        mode=$(tr -d '[:space:]' < "$IR_MODE_FILE" | tr '[:upper:]' '[:lower:]')
        case "$mode" in
            on|off|auto|time)
                echo "$mode"
                return
                ;;
            *)
                log_warn "Unknown mode in $IR_MODE_FILE: '$mode' — using default"
                ;;
        esac
    fi
    echo "$IR_DEFAULT_MODE"
}

# ---------------------------------------------------------------------------
# check_brightness — estimate ambient brightness from camera
#
# Method 1: v4l2-ctl to read auto-exposure result (mean luminance).
#           v4l2-ctl --device=/dev/video0 --get-ctrl=mean_luma
#           (requires kernel driver that exposes this control)
#
# Method 2: Fallback — read exposure setting as brightness proxy.
#           Lower exposure value = camera compensating for darkness.
#
# Returns an integer 0-255 via stdout.
# ---------------------------------------------------------------------------

check_brightness() {
    local brightness=128   # Default: assume medium brightness

    if [ ! -e "$CAMERA_DEVICE" ]; then
        echo "$brightness"
        return
    fi

    # Try reading mean luminance (IMX219 with libcamera exposes this)
    local luma
    luma=$(v4l2-ctl --device="$CAMERA_DEVICE" \
                    --get-ctrl=mean_luma 2>/dev/null \
           | grep -oP '(?<=: )\d+' || true)

    if [ -n "$luma" ]; then
        echo "$luma"
        return
    fi

    # Fallback: read auto-exposure absolute value (inverse proxy for brightness)
    # Higher exposure = darker environment.  Invert to 0-255 scale.
    local exposure
    exposure=$(v4l2-ctl --device="$CAMERA_DEVICE" \
                        --get-ctrl=exposure_absolute 2>/dev/null \
               | grep -oP '(?<=: )\d+' || true)

    if [ -n "$exposure" ]; then
        # exposure_absolute typical range 1-10000; 1 = bright, 10000 = dark
        # Map to 0-255 inverted: brightness = 255 * (1 - exposure/10000)
        brightness=$(awk -v e="$exposure" \
                         'BEGIN { v=int(255*(1-e/10000)); print (v<0?0:(v>255?255:v)) }')
        echo "$brightness"
        return
    fi

    # No usable camera metadata — return default
    echo "$brightness"
}

# ---------------------------------------------------------------------------
# Time-based mode check
#
# Returns 0 (should be ON) or 1 (should be OFF)
# ---------------------------------------------------------------------------

time_mode_should_be_on() {
    local hour
    hour=$(date +%H)
    # Remove leading zeros to avoid octal interpretation
    hour=${hour#0}
    hour=${hour:-0}

    local on_hour=${IR_TIME_ON_HOUR#0}
    local off_hour=${IR_TIME_OFF_HOUR#0}
    on_hour=${on_hour:-0}
    off_hour=${off_hour:-0}

    if [ "$on_hour" -gt "$off_hour" ]; then
        # Spans midnight: ON from on_hour to midnight, midnight to off_hour
        [ "$hour" -ge "$on_hour" ] || [ "$hour" -lt "$off_hour" ]
    else
        # Same-day range (unusual but handle it)
        [ "$hour" -ge "$on_hour" ] && [ "$hour" -lt "$off_hour" ]
    fi
}

# ---------------------------------------------------------------------------
# Cleanup on exit
# ---------------------------------------------------------------------------

cleanup() {
    log_info "Shutting down — turning IR LEDs off"
    gpio_write 0 2>/dev/null || true

    if [ "$IR_GPIO_NUM" != "XXX" ] && [ -e "$IR_GPIO_DIR" ]; then
        echo "$IR_GPIO_NUM" > "$IR_GPIO_UNEXPORT" 2>/dev/null || true
    fi
}

trap cleanup EXIT SIGTERM SIGINT

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

log_info "IR LED control starting"
log_info "  GPIO:        ${IR_GPIO_NUM}"
log_info "  Default mode: ${IR_DEFAULT_MODE}"
log_info "  Time on:     ${IR_TIME_ON_HOUR}:00"
log_info "  Time off:    ${IR_TIME_OFF_HOUR}:00"
log_info "  Threshold:   ${IR_BRIGHTNESS_THRESHOLD}/255"
log_info "  Camera:      ${CAMERA_DEVICE}"

gpio_setup

while true; do
    mode=$(get_current_mode)

    case "$mode" in
        on)
            ir_on
            sleep 5
            ;;

        off)
            ir_off
            sleep 5
            ;;

        time)
            if time_mode_should_be_on; then
                ir_on
            else
                ir_off
            fi
            sleep 60
            ;;

        auto)
            brightness=$(check_brightness)
            log_info "Auto mode: brightness=${brightness} threshold=${IR_BRIGHTNESS_THRESHOLD}"

            if [ "$brightness" -lt "$IR_BRIGHTNESS_THRESHOLD" ]; then
                ir_on
            else
                ir_off
            fi
            sleep "$IR_AUTO_CHECK_INTERVAL"
            ;;

        *)
            log_warn "Unrecognised mode '$mode' — defaulting to off"
            ir_off
            sleep 10
            ;;
    esac
done

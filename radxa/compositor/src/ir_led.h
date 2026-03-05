#pragma once
/*
 * ir_led.h — IR LED control via GPIO with auto-brightness detection
 *
 * Controls 850nm IR LEDs wired to a Radxa Zero 3W GPIO pin.
 * Three modes:
 *   auto — sample camera frame brightness, toggle LEDs when dark
 *   on   — always on
 *   off  — always off
 *
 * In auto mode, call ir_led_update() with each camera NV12 frame.
 * The module subsamples the Y plane to compute average luminance and
 * toggles the GPIO at most once per second to avoid flicker.
 */

#include <stdint.h>
#include <stddef.h>

typedef enum {
    IR_MODE_OFF  = 0,
    IR_MODE_ON   = 1,
    IR_MODE_AUTO = 2,
} ir_mode_t;

/**
 * Initialise GPIO for IR LED control.
 * gpio_num: Linux GPIO number (124 = GPIO3_D4 on Radxa Zero 3W).
 * Returns 0 on success, -1 on failure (non-fatal — runs in dry-run mode).
 */
int ir_led_init(int gpio_num);

/**
 * Set IR LED mode. Takes effect on next ir_led_update() call,
 * or immediately for on/off modes.
 */
void ir_led_set_mode(ir_mode_t mode);

/**
 * Get current IR LED mode.
 */
ir_mode_t ir_led_get_mode(void);

/**
 * Returns 1 if LEDs are currently on, 0 if off.
 */
int ir_led_is_on(void);

/**
 * Feed a camera frame for auto-brightness detection.
 * nv12_data: pointer to NV12 frame (Y plane is first w*h bytes).
 * w, h: frame dimensions.
 *
 * In auto mode, computes average luminance from a subsampled grid
 * of the Y plane. Toggles LEDs on/off based on brightness threshold.
 * Rate-limited to one check per second to avoid flicker.
 *
 * In on/off mode, this is a no-op (LEDs stay in forced state).
 */
void ir_led_update(const uint8_t *nv12_data, int w, int h);

/**
 * Clean up: turn LEDs off, unexport GPIO.
 */
void ir_led_destroy(void);

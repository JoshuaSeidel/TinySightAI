#pragma once

#include "mode.h"
#include <stdbool.h>

/*
 * Touch coordinate remapping for split-screen and overlay buttons.
 *
 * Car touchscreen sends AA touch events with coordinates normalized to
 * the display resolution (1280x720). We remap based on display mode:
 *
 * Full mode:  Pass through (forward to phone)
 * Split mode: Left half → remap to full phone coords
 *             Right half → camera zoom/pan (handle locally)
 * Button zones: [⊞] mode cycle, [+] zoom in, [-] zoom out
 */

/* Touch button zone (pixel coordinates at 1280x720) */
#define BTN_MODE_X      1220    /* [⊞] bottom-right corner */
#define BTN_MODE_Y      660
#define BTN_MODE_SIZE   50

#define BTN_ZOOM_IN_X   1150    /* [+] */
#define BTN_ZOOM_IN_Y   660
#define BTN_ZOOM_SIZE   40

#define BTN_ZOOM_OUT_X  1090    /* [-] */
#define BTN_ZOOM_OUT_Y  660

typedef enum {
    TOUCH_FORWARD_AA,     /* Forward to Android Auto phone */
    TOUCH_FORWARD_CP,     /* Forward to CarPlay phone */
    TOUCH_MODE_CYCLE,     /* Handled: cycle display mode */
    TOUCH_ZOOM_IN,        /* Handled: camera zoom in */
    TOUCH_ZOOM_OUT,       /* Handled: camera zoom out */
    TOUCH_MODE_TOGGLE,    /* Handled: toggle AA/CarPlay source */
    TOUCH_CAMERA_LOCAL,   /* Handled: touch on camera region (ignore) */
} touch_action_t;

/**
 * Process a touch event and determine what to do with it.
 * x, y are in display coordinates (0-1279, 0-719).
 * If action is TOUCH_FORWARD_*, out_x/out_y contain remapped coordinates.
 */
touch_action_t touch_process(const display_state_t *state,
                              int x, int y,
                              int *out_x, int *out_y);

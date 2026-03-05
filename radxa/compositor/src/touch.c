#include "touch.h"

static bool in_rect(int x, int y, int rx, int ry, int size)
{
    return (x >= rx && x < rx + size && y >= ry && y < ry + size);
}

touch_action_t touch_process(const display_state_t *state,
                              int x, int y,
                              int *out_x, int *out_y)
{
    /* Check button zones first (overlay buttons, always active) */
    if (in_rect(x, y, BTN_MODE_X, BTN_MODE_Y, BTN_MODE_SIZE))
        return TOUCH_MODE_CYCLE;

    /* Zoom buttons only visible in split or camera-only mode */
    if (state->layout != LAYOUT_FULL_PRIMARY) {
        if (in_rect(x, y, BTN_ZOOM_IN_X, BTN_ZOOM_IN_Y, BTN_ZOOM_SIZE))
            return TOUCH_ZOOM_IN;
        if (in_rect(x, y, BTN_ZOOM_OUT_X, BTN_ZOOM_OUT_Y, BTN_ZOOM_SIZE))
            return TOUCH_ZOOM_OUT;
    }

    switch (state->layout) {
    case LAYOUT_FULL_PRIMARY:
        /* All touches go to the phone */
        *out_x = x;
        *out_y = y;
        return (state->source == SOURCE_AA) ? TOUCH_FORWARD_AA : TOUCH_FORWARD_CP;

    case LAYOUT_SPLIT_LEFT_RIGHT:
        if (x < 640) {
            /* Left half — phone. Remap 0-639 → 0-1279 */
            *out_x = x * 2;
            *out_y = y;
            return (state->source == SOURCE_AA) ? TOUCH_FORWARD_AA : TOUCH_FORWARD_CP;
        } else {
            /* Right half — camera region, handle locally */
            return TOUCH_CAMERA_LOCAL;
        }

    case LAYOUT_FULL_CAMERA:
        /* All touches are local (camera pan/zoom) */
        return TOUCH_CAMERA_LOCAL;
    }

    *out_x = x;
    *out_y = y;
    return TOUCH_FORWARD_AA;
}

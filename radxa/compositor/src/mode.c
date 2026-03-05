#include "mode.h"
#include "camera.h"
#include <stdio.h>

void mode_init(display_state_t *state)
{
    state->layout = LAYOUT_FULL_PRIMARY;
    state->source = SOURCE_AA;
    state->cam_zoom = 1.0f;
}

void mode_cycle(display_state_t *state)
{
    switch (state->layout) {
    case LAYOUT_FULL_PRIMARY:
        state->layout = LAYOUT_SPLIT_LEFT_RIGHT;
        break;
    case LAYOUT_SPLIT_LEFT_RIGHT:
        state->layout = LAYOUT_FULL_CAMERA;
        break;
    case LAYOUT_FULL_CAMERA:
        state->layout = LAYOUT_FULL_PRIMARY;
        break;
    }
    printf("mode: layout=%d source=%d zoom=%.1f\n",
           state->layout, state->source, state->cam_zoom);
}

void mode_toggle_source(display_state_t *state)
{
    state->source = (state->source == SOURCE_AA) ? SOURCE_CARPLAY : SOURCE_AA;
    printf("mode: source toggled to %s\n",
           state->source == SOURCE_AA ? "Android Auto" : "CarPlay");
}

void mode_zoom_in(display_state_t *state)
{
    state->cam_zoom += CAM_ZOOM_STEP;
    if (state->cam_zoom > CAM_MAX_ZOOM)
        state->cam_zoom = CAM_MAX_ZOOM;
    printf("mode: zoom in → %.1fx\n", state->cam_zoom);
}

void mode_zoom_out(display_state_t *state)
{
    state->cam_zoom -= CAM_ZOOM_STEP;
    if (state->cam_zoom < 1.0f)
        state->cam_zoom = 1.0f;
    printf("mode: zoom out → %.1fx\n", state->cam_zoom);
}

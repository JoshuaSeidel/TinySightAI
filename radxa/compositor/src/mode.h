#pragma once

#include "pipeline.h"

/*
 * Display mode state machine.
 * Manages which content source is active and the layout.
 */

typedef enum {
    SOURCE_AA,
    SOURCE_CARPLAY,
} primary_source_t;

typedef struct {
    layout_mode_t layout;
    primary_source_t source;
    float cam_zoom;
} display_state_t;

/**
 * Initialize display state with defaults (Full AA, zoom 1.0).
 */
void mode_init(display_state_t *state);

/**
 * Cycle to next display mode.
 * Order: Full AA → Split AA+Cam → Full Cam → Full AA
 */
void mode_cycle(display_state_t *state);

/**
 * Toggle primary source between AA and CarPlay.
 */
void mode_toggle_source(display_state_t *state);

/**
 * Adjust camera zoom.
 */
void mode_zoom_in(display_state_t *state);
void mode_zoom_out(display_state_t *state);

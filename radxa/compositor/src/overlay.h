/*
 * overlay.h — RGA-based on-screen controls overlay
 *
 * Pre-renders small icon bitmaps into NV12 buffers at init time and
 * composites them onto the output frame using RGA SRC-over alpha blending.
 *
 * Icons rendered programmatically (no image file dependency):
 *
 *   Mode button [⊞]  — 2×2 grid of squares, top-right corner
 *   Zoom-in  [+]     — circle with horizontal/vertical bar
 *   Zoom-out [-]     — circle with horizontal bar only
 *
 * All icons are 48×48 pixels, white foreground on a semi-transparent
 * dark rectangle (RGBA 0,0,0,128).
 *
 * The overlay is applied after pipeline_composite() but before the
 * encoded H.264 is sent to the car.
 *
 * Pixel format: NV12 (YUV 4:2:0 semi-planar) — matches the compositor
 * pipeline composite buffer.  Alpha for the background rectangle is
 * emulated by blending manually into the YUV frame because standard
 * RGA SRC-OVER expects an RGBA source; we apply the background in
 * software and the foreground (white) via RGA blit.
 */
#pragma once

#include "pipeline.h"   /* layout_mode_t */
#include <rga/im2d.h>   /* rga_buffer_t */

/* Icon pixel dimensions */
#define OVERLAY_ICON_W  48
#define OVERLAY_ICON_H  48

/* Margin from frame edge (pixels) */
#define OVERLAY_MARGIN  16

/* Background alpha (0-255). 0 = transparent, 255 = opaque. */
#define OVERLAY_BG_ALPHA 160

/**
 * Initialise overlay subsystem.
 * Pre-renders icon bitmaps into internal RGBA buffers.
 * Must be called once before any other overlay_* function.
 * Returns 0 on success, -1 on failure.
 */
int overlay_init(void);

/**
 * Composite overlay controls onto an NV12 frame buffer.
 *
 * @param frame_data  Pointer to NV12 frame data (writable, in-place edit).
 * @param frame_w     Frame width in pixels (should be 1280).
 * @param frame_h     Frame height in pixels (should be 720).
 * @param layout      Current display layout — controls which icons appear.
 * @param show_controls  If 0, only the mode button is drawn; if 1, zoom
 *                       buttons are also drawn (for camera/split modes).
 */
void overlay_render(uint8_t *frame_data, int frame_w, int frame_h,
                    layout_mode_t layout, int show_controls);

/**
 * Globally show or hide all overlay icons.
 * When hidden, overlay_render() is a no-op.
 */
void overlay_set_visible(int visible);

/**
 * Free all pre-rendered icon buffers allocated during overlay_init().
 */
void overlay_destroy(void);

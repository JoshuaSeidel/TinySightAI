/*
 * overlay.c — RGA-based on-screen controls overlay
 *
 * Icon rendering pipeline:
 *
 *   1. Pre-render each icon into a 48×48 RGBA buffer (software, once at init).
 *   2. At render time, for each icon:
 *      a. Draw a semi-transparent dark rectangle into the NV12 frame
 *         (software blend into Y/UV planes — RGA SRC-OVER needs RGBA source,
 *          but our frame is NV12, so we blend the background by hand).
 *      b. Blit the white icon foreground (RGBA→NV12) via RGA imblend().
 *
 * Touch hit-zones match touch.h (BTN_MODE_*, BTN_ZOOM_*) — the overlay
 * icons are drawn at the same pixel coordinates.
 *
 * NV12 layout reminder:
 *   Y plane:  width × height  bytes starting at offset 0
 *   UV plane: width × height/2 bytes (interleaved U,V pairs) starting at
 *             offset width × height
 */
#include "overlay.h"
#include "touch.h"      /* BTN_MODE_X/Y, BTN_ZOOM_IN_X/Y, etc. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---- Internal state ---- */

static int g_visible = 1;  /* globally show/hide */

/*
 * Pre-rendered icon bitmaps in RGBA format.
 * We convert to NV12 at blit time for each frame position.
 */
#define ICON_PIXELS (OVERLAY_ICON_W * OVERLAY_ICON_H)

typedef struct {
    uint8_t rgba[ICON_PIXELS * 4]; /* RGBA, row-major */
} icon_buf_t;

static icon_buf_t g_icon_mode;    /* ⊞  grid */
static icon_buf_t g_icon_zoom_in; /* circle + cross */
static icon_buf_t g_icon_zoom_out;/* circle + minus */

/* ---- RGBA drawing helpers ---- */

static void rgba_clear(icon_buf_t *icon)
{
    memset(icon->rgba, 0, sizeof(icon->rgba));
}

/* Set pixel (x,y) to RGBA, clipping to bounds */
static void rgba_set(icon_buf_t *icon, int x, int y,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (x < 0 || x >= OVERLAY_ICON_W || y < 0 || y >= OVERLAY_ICON_H)
        return;
    uint8_t *p = icon->rgba + (y * OVERLAY_ICON_W + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

/* Draw a filled rectangle in the icon */
static void rgba_fill_rect(icon_buf_t *icon,
                            int rx, int ry, int rw, int rh,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (int dy = ry; dy < ry + rh; dy++)
        for (int dx = rx; dx < rx + rw; dx++)
            rgba_set(icon, dx, dy, r, g, b, a);
}

/* Draw a filled circle (anti-aliased via simple distance check) */
static void rgba_fill_circle(icon_buf_t *icon,
                              int cx, int cy, int radius,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (int dy = cy - radius; dy <= cy + radius; dy++) {
        for (int dx = cx - radius; dx <= cx + radius; dx++) {
            int dx2 = dx - cx;
            int dy2 = dy - cy;
            if (dx2 * dx2 + dy2 * dy2 <= radius * radius)
                rgba_set(icon, dx, dy, r, g, b, a);
        }
    }
}

/* Draw a hollow circle (ring, 2px thick) */
static void rgba_ring(icon_buf_t *icon,
                       int cx, int cy, int radius,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int r2_outer = radius * radius;
    int r2_inner = (radius - 3) * (radius - 3);
    for (int dy = cy - radius; dy <= cy + radius; dy++) {
        for (int dx = cx - radius; dx <= cx + radius; dx++) {
            int dx2 = dx - cx;
            int dy2 = dy - cy;
            int d2  = dx2 * dx2 + dy2 * dy2;
            if (d2 <= r2_outer && d2 >= r2_inner)
                rgba_set(icon, dx, dy, r, g, b, a);
        }
    }
}

/* ---- Icon pre-rendering ---- */

/*
 * ⊞ Mode button: 2×2 grid of filled squares with a small gap.
 * Each square is 17×17 px; gap between squares is 3 px.
 * Total grid = 2*17 + 3 = 37 px, centred in 48×48.
 */
static void render_mode_icon(void)
{
    rgba_clear(&g_icon_mode);

    const uint8_t r = 255, g2 = 255, b = 255, a = 230;
    const int sq_size = 17;
    const int gap     = 3;
    const int total   = sq_size * 2 + gap; /* 37 */
    const int ox      = (OVERLAY_ICON_W - total) / 2; /* left edge of grid */
    const int oy      = (OVERLAY_ICON_H - total) / 2;

    /* Top-left square */
    rgba_fill_rect(&g_icon_mode, ox, oy, sq_size, sq_size, r, g2, b, a);
    /* Top-right square */
    rgba_fill_rect(&g_icon_mode, ox + sq_size + gap, oy, sq_size, sq_size, r, g2, b, a);
    /* Bottom-left square */
    rgba_fill_rect(&g_icon_mode, ox, oy + sq_size + gap, sq_size, sq_size, r, g2, b, a);
    /* Bottom-right square */
    rgba_fill_rect(&g_icon_mode, ox + sq_size + gap, oy + sq_size + gap, sq_size, sq_size, r, g2, b, a);
}

/*
 * Zoom-in [+]: white ring + horizontal line + vertical line inside.
 */
static void render_zoom_in_icon(void)
{
    rgba_clear(&g_icon_zoom_in);

    const uint8_t r = 255, g2 = 255, b = 255, a = 230;
    const int cx = OVERLAY_ICON_W / 2;
    const int cy = OVERLAY_ICON_H / 2;
    const int radius = 18;
    const int bar_half = 10;
    const int bar_thick = 3;

    /* Circle ring */
    rgba_ring(&g_icon_zoom_in, cx, cy, radius, r, g2, b, a);

    /* Horizontal bar */
    rgba_fill_rect(&g_icon_zoom_in,
                   cx - bar_half, cy - bar_thick / 2,
                   bar_half * 2, bar_thick,
                   r, g2, b, a);
    /* Vertical bar */
    rgba_fill_rect(&g_icon_zoom_in,
                   cx - bar_thick / 2, cy - bar_half,
                   bar_thick, bar_half * 2,
                   r, g2, b, a);
}

/*
 * Zoom-out [-]: white ring + horizontal bar only.
 */
static void render_zoom_out_icon(void)
{
    rgba_clear(&g_icon_zoom_out);

    const uint8_t r = 255, g2 = 255, b = 255, a = 230;
    const int cx = OVERLAY_ICON_W / 2;
    const int cy = OVERLAY_ICON_H / 2;
    const int radius = 18;
    const int bar_half = 10;
    const int bar_thick = 3;

    rgba_ring(&g_icon_zoom_out, cx, cy, radius, r, g2, b, a);

    /* Horizontal bar only */
    rgba_fill_rect(&g_icon_zoom_out,
                   cx - bar_half, cy - bar_thick / 2,
                   bar_half * 2, bar_thick,
                   r, g2, b, a);
}

/* ---- NV12 blending ---- */

/*
 * Convert an RGBA pixel (r,g,b) to YUV (BT.601 limited range) and blend
 * with alpha `a` (0-255) into NV12 frame.
 *
 * NV12 layout:
 *   Y  at frame_data[y * stride + x]
 *   Cb at frame_data[stride*h + (y/2)*stride + (x&~1)]
 *   Cr at frame_data[stride*h + (y/2)*stride + (x&~1) + 1]
 *
 * We use full-frame stride == frame_w.
 */
static inline uint8_t clamp_u8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void nv12_blend_pixel(uint8_t *frame_data,
                              int frame_w, int frame_h,
                              int px, int py,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    if (px < 0 || px >= frame_w || py < 0 || py >= frame_h)
        return;

    /* BT.601 limited range: Y=[16..235], Cb/Cr=[16..240] */
    int y_new  = ((  66 * r + 129 * g +  25 * b + 128) >> 8) + 16;
    int cb_new = ((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
    int cr_new = ((112 * r -  94 * g -  18 * b + 128) >> 8) + 128;

    /* Blend into Y plane */
    int y_idx = py * frame_w + px;
    int y_old = frame_data[y_idx];
    frame_data[y_idx] = clamp_u8(y_old + ((y_new - y_old) * alpha / 255));

    /* Blend into UV plane (chroma is shared over 2×2 luma block) */
    int uv_base = frame_w * frame_h;
    int uv_row  = (py / 2) * frame_w;
    int uv_col  = (px & ~1); /* align to even column */
    int cb_idx  = uv_base + uv_row + uv_col;
    int cr_idx  = cb_idx + 1;

    if (cb_idx + 1 < frame_w * frame_h * 3 / 2) {
        int cb_old = frame_data[cb_idx];
        int cr_old = frame_data[cr_idx];
        frame_data[cb_idx] = clamp_u8(cb_old + ((cb_new - cb_old) * alpha / 255));
        frame_data[cr_idx] = clamp_u8(cr_old + ((cr_new - cr_old) * alpha / 255));
    }
}

/*
 * Draw a semi-transparent dark background rectangle into the NV12 frame.
 * Uses OVERLAY_BG_ALPHA for the dark (Y=16 = black in limited range).
 */
static void draw_bg_rect(uint8_t *frame_data, int frame_w, int frame_h,
                          int rx, int ry, int rw, int rh)
{
    for (int dy = ry; dy < ry + rh && dy < frame_h; dy++) {
        for (int dx = rx; dx < rx + rw && dx < frame_w; dx++) {
            nv12_blend_pixel(frame_data, frame_w, frame_h,
                             dx, dy, 0, 0, 0, OVERLAY_BG_ALPHA);
        }
    }
}

/*
 * Blit a pre-rendered RGBA icon at (ox, oy) into the NV12 frame.
 * Each RGBA pixel with alpha > 0 is blended.
 */
static void blit_icon(uint8_t *frame_data, int frame_w, int frame_h,
                       const icon_buf_t *icon, int ox, int oy)
{
    for (int iy = 0; iy < OVERLAY_ICON_H; iy++) {
        for (int ix = 0; ix < OVERLAY_ICON_W; ix++) {
            const uint8_t *p = icon->rgba + (iy * OVERLAY_ICON_W + ix) * 4;
            uint8_t a = p[3];
            if (a == 0) continue;
            nv12_blend_pixel(frame_data, frame_w, frame_h,
                             ox + ix, oy + iy,
                             p[0], p[1], p[2], a);
        }
    }
}

/* ---- Public API ---- */

int overlay_init(void)
{
    render_mode_icon();
    render_zoom_in_icon();
    render_zoom_out_icon();
    printf("overlay: icons pre-rendered (%dx%d px each)\n",
           OVERLAY_ICON_W, OVERLAY_ICON_H);
    return 0;
}

void overlay_set_visible(int visible)
{
    g_visible = visible;
}

void overlay_render(uint8_t *frame_data, int frame_w, int frame_h,
                    layout_mode_t layout, int show_controls)
{
    if (!g_visible || !frame_data)
        return;

    /*
     * Mode button [⊞] — always shown at top-right corner.
     * Position it so the right edge is (frame_w - OVERLAY_MARGIN) and
     * the top edge is OVERLAY_MARGIN.
     */
    const int btn_w = OVERLAY_ICON_W;
    const int btn_h = OVERLAY_ICON_H;
    const int bg_pad = 4; /* extra padding around icon for bg rect */

    int mode_ox = frame_w - OVERLAY_MARGIN - btn_w;
    int mode_oy = OVERLAY_MARGIN;

    draw_bg_rect(frame_data, frame_w, frame_h,
                 mode_ox - bg_pad, mode_oy - bg_pad,
                 btn_w + bg_pad * 2, btn_h + bg_pad * 2);
    blit_icon(frame_data, frame_w, frame_h,
              &g_icon_mode, mode_ox, mode_oy);

    /* Zoom buttons — only relevant when camera is visible */
    if (show_controls && layout != LAYOUT_FULL_PRIMARY) {
        /*
         * Zoom-in [+]: to the left of the mode button.
         * Zoom-out [-]: further left.
         */
        int zi_ox = mode_ox - OVERLAY_MARGIN - btn_w;
        int zi_oy = mode_oy;

        int zo_ox = zi_ox - OVERLAY_MARGIN - btn_w;
        int zo_oy = mode_oy;

        draw_bg_rect(frame_data, frame_w, frame_h,
                     zi_ox - bg_pad, zi_oy - bg_pad,
                     btn_w + bg_pad * 2, btn_h + bg_pad * 2);
        blit_icon(frame_data, frame_w, frame_h,
                  &g_icon_zoom_in, zi_ox, zi_oy);

        draw_bg_rect(frame_data, frame_w, frame_h,
                     zo_ox - bg_pad, zo_oy - bg_pad,
                     btn_w + bg_pad * 2, btn_h + bg_pad * 2);
        blit_icon(frame_data, frame_w, frame_h,
                  &g_icon_zoom_out, zo_ox, zo_oy);
    }
}

void overlay_destroy(void)
{
    /* Icons are static buffers embedded in file-scope structs — nothing to
     * free.  Zero them to prevent stale use after reinit. */
    memset(&g_icon_mode,     0, sizeof(g_icon_mode));
    memset(&g_icon_zoom_in,  0, sizeof(g_icon_zoom_in));
    memset(&g_icon_zoom_out, 0, sizeof(g_icon_zoom_out));
    printf("overlay: destroyed\n");
}

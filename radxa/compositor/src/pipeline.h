#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Hardware video pipeline using RK3566 MPP (Media Process Platform).
 *
 * Flow:
 *   Input H.264/H.265 (from AA/CarPlay) → RKVDEC2 decode → YUV frame
 *   Camera NV12 → (already YUV)
 *   Both YUV frames → RGA composite → single YUV frame
 *   Composited YUV → Hantro H1 encode → Output H.264
 *
 * Decode: H.264 AND H.265 supported (RKVDEC2 handles both)
 * Encode: H.264 ONLY (Hantro H1 has no H.265 encode)
 * Output to car is always H.264 — Android Auto protocol requires it.
 *
 * All operations are hardware-accelerated. Target: <20ms at 720p 30fps.
 */

typedef struct pipeline pipeline_t;

typedef enum {
    LAYOUT_FULL_PRIMARY,   /* AA or CarPlay fullscreen */
    LAYOUT_FULL_CAMERA,    /* Baby cam fullscreen */
    LAYOUT_SPLIT_LEFT_RIGHT, /* Primary left (640x720), Camera right (640x720) */
} layout_mode_t;

typedef enum {
    CODEC_H264,   /* AVC — Android Auto always sends this */
    CODEC_H265,   /* HEVC — CarPlay/AirPlay may send this from newer iPhones */
} input_codec_t;

/**
 * Initialize the hardware video pipeline.
 * Sets up MPP decoder(s), RGA compositor, and MPP encoder.
 * Output is always 1280x720 H.264 (car protocol requirement).
 */
pipeline_t *pipeline_init(int output_w, int output_h, int fps);

/**
 * Decode a video frame from the phone (AA or CarPlay).
 * Accepts both H.264 and H.265 input — RKVDEC2 handles both.
 * Returns 0 on success. Decoded YUV is stored internally.
 */
int pipeline_decode(pipeline_t *p, const uint8_t *data, size_t len,
                     input_codec_t codec);

/**
 * Composite the decoded phone frame with a camera NV12 frame.
 * Layout determines the arrangement.
 * If camera_data is NULL, only the phone frame is used.
 */
int pipeline_composite(pipeline_t *p, layout_mode_t layout,
                        const uint8_t *camera_nv12, int cam_w, int cam_h);

/**
 * Encode the composited frame to H.264.
 * Returns pointer to encoded data and sets out_len.
 * Pointer is valid until next call to pipeline_encode.
 */
const uint8_t *pipeline_encode(pipeline_t *p, size_t *out_len);

/**
 * Pass through a raw camera NV12 frame (no phone video).
 * Encodes directly to H.264.
 */
const uint8_t *pipeline_encode_camera_only(pipeline_t *p,
    const uint8_t *nv12, int w, int h, size_t *out_len);

/**
 * Clean up all hardware resources.
 */
void pipeline_destroy(pipeline_t *p);

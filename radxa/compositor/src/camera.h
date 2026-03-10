#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * V4L2 camera capture for IMX219 NoIR on Radxa Zero 3W.
 * Outputs NV12 frames via V4L2 crop + capture.
 * Digital zoom is implemented via V4L2 selection API (crop region).
 */

#define CAM_SENSOR_W     3280
#define CAM_SENSOR_H     2464
#define CAM_OUTPUT_W     1280
#define CAM_OUTPUT_H     720
#define CAM_FPS          30
#define CAM_PIXEL_FMT    0x3231564E  /* NV12 */

#define CAM_MAX_ZOOM     3.4f  /* 3280/960 ≈ 3.4x at 720p */
#define CAM_ZOOM_STEP    0.2f

typedef struct {
    int fd;
    int width;
    int height;
    float zoom_level;
    uint8_t *buffers[4];
    size_t buffer_sizes[4];
    int num_buffers;
} camera_t;

/**
 * Open and initialize the V4L2 camera device.
 * Sets up capture at CAM_OUTPUT_W x CAM_OUTPUT_H @ CAM_FPS.
 */
int camera_init(camera_t *cam, const char *device);

/**
 * Start streaming.
 */
int camera_start(camera_t *cam);

/**
 * Dequeue a frame. Returns buffer index, or -1 on error.
 * frame_data and frame_len are set to the NV12 data.
 */
int camera_dequeue(camera_t *cam, uint8_t **frame_data, size_t *frame_len);

/**
 * Requeue a buffer after processing.
 */
int camera_enqueue(camera_t *cam, int buf_idx);

/**
 * Set digital zoom level (1.0 = no zoom, 3.4 = max).
 * Adjusts the V4L2 crop selection on the sensor.
 */
int camera_set_zoom(camera_t *cam, float zoom);

/**
 * Stop streaming and close device.
 */
void camera_close(camera_t *cam);

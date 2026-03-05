/*
 * carplay_input.h — Accept H.264/H.265 video from the CarPlay AirPlay receiver
 *
 * The CarPlay AirPlay receiver (RPiPlay/shairplay derivative) runs as a
 * separate process and delivers decoded or raw H.264/H.265 NAL data via a
 * Unix domain socket.
 *
 * Wire format (socket stream):
 *   [4-byte big-endian length][length bytes of H.264/H.265 Annex B data]
 *
 * This module:
 *   - Listens on /tmp/carplay-video.sock for the AirPlay receiver to connect
 *   - Reads framed video data in a background thread
 *   - Parses the NAL header to auto-detect codec (H.264 or H.265)
 *   - Stores the latest frame in a double-buffered slot
 *   - Exposes a non-blocking getter for the compositor main loop
 *
 * Thread safety:
 *   carplay_input_get_frame() may be called from any thread; internally
 *   protected by a mutex.
 */
#pragma once

#include "pipeline.h"   /* input_codec_t */
#include <stdint.h>
#include <stddef.h>

#define CARPLAY_SOCK_PATH    "/tmp/carplay-video.sock"
#define CARPLAY_MAX_FRAME    (2 * 1024 * 1024)  /* 2 MB max NAL frame */

/**
 * Initialise the CarPlay input listener.
 * Creates the Unix domain socket and starts an accept/receive thread.
 * Returns 0 on success, -1 on error.
 */
int carplay_input_init(void);

/**
 * Get the latest received CarPlay video frame (non-blocking).
 *
 * On success (return 0):
 *   *data  points to an internal buffer valid until the NEXT call to
 *          carplay_input_get_frame().
 *   *len   is the data length in bytes.
 *   *codec is CODEC_H264 or CODEC_H265.
 *
 * Returns -1 if no new frame is available since the last call.
 *
 * The caller must NOT free or modify *data.
 */
int carplay_input_get_frame(const uint8_t **data, size_t *len,
                             input_codec_t *codec);

/**
 * Stop the receiver thread, close socket, free buffers.
 */
void carplay_input_destroy(void);

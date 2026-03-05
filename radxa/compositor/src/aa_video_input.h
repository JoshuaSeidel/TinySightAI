/*
 * aa_video_input.h — Receive tapped AA video frames from aa-proxy
 *
 * The aa-proxy (Rust) runs as the actual AAP relay between the T-Dongle and
 * the phone. It intercepts channel-3 (video) frames and taps a COPY of each
 * raw H.264/H.265 NAL payload to /tmp/aa-video.sock so the compositor can
 * decode, composite with camera, and re-encode.
 *
 * Wire format (same as carplay_input):
 *   [4-byte big-endian length][length bytes of raw H.264/H.265 Annex B data]
 *
 * This module:
 *   - Listens on /tmp/aa-video.sock (server) for aa-proxy to connect
 *   - Reads length-prefixed NAL frames in a background thread
 *   - Auto-detects codec via nal_detect_codec()
 *   - Stores the latest frame in a double-buffered slot
 *   - Exposes a non-blocking getter used by the aa video thread in main.c
 *
 * Thread safety:
 *   aa_video_input_get_frame() may be called from any thread; internally
 *   protected by a mutex. The frame pointer is valid until the NEXT call.
 */
#pragma once

#include "pipeline.h"   /* input_codec_t */
#include <stdint.h>
#include <stddef.h>

#define AA_VIDEO_SOCK_PATH   "/tmp/aa-video.sock"
#define AA_VIDEO_MAX_FRAME   (2 * 1024 * 1024)  /* 2 MB max NAL frame */

/**
 * Initialise the AA video tap listener.
 * Creates the Unix domain socket and starts an accept/receive thread.
 * Returns 0 on success, -1 on error.
 */
int aa_video_input_init(void);

/**
 * Get the latest received AA video frame (non-blocking).
 *
 * On success (return 0):
 *   *data  points to an internal buffer valid until the NEXT call to
 *          aa_video_input_get_frame().
 *   *len   is the data length in bytes.
 *   *codec is CODEC_H264 or CODEC_H265.
 *
 * Returns -1 if no new frame is available since the last call.
 *
 * The caller must NOT free or modify *data.
 */
int aa_video_input_get_frame(const uint8_t **data, size_t *len,
                              input_codec_t *codec);

/**
 * Stop the receiver thread, close socket, free buffers.
 */
void aa_video_input_destroy(void);

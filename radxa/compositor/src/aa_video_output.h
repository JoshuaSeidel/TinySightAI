/*
 * aa_video_output.h — Send composited H.264 frames back to aa-proxy
 *
 * After the compositor receives an AA video tap frame from aa-proxy via
 * /tmp/aa-video.sock, decodes it, composites with the camera feed, and
 * re-encodes to H.264, it delivers the result to aa-proxy via this module.
 *
 * Transport:
 *   aa-proxy listens on /tmp/aa-video-out.sock (it is the server).
 *   The compositor connects to it as a client and streams composited frames.
 *   aa-proxy reads from this socket and substitutes the original channel-3
 *   NAL data with the composited output before forwarding to the car.
 *
 * Wire format (same as aa_video_input / carplay_input — length-prefixed):
 *   [4-byte big-endian length][length bytes of H.264 Annex B data]
 *
 * Reconnect behaviour:
 *   If aa-proxy is not yet running or closes the connection, the output
 *   module retries the connection in the background. Frames written while
 *   disconnected are silently dropped so the compositor pipeline never
 *   blocks.
 *
 * Thread safety:
 *   aa_video_output_send_frame() is safe to call from any thread.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define AA_VIDEO_OUT_SOCK_PATH  "/tmp/aa-video-out.sock"

/**
 * Initialise the output channel.
 * Starts a background reconnect thread that attempts to connect to
 * /tmp/aa-video-out.sock (aa-proxy's listener).
 * Returns 0 always; actual connection happens asynchronously.
 */
int aa_video_output_init(void);

/**
 * Send a composited H.264 frame to aa-proxy.
 *
 * @param data    Pointer to H.264 Annex B data.
 * @param len     Length in bytes.
 *
 * Writes the 4-byte big-endian length header followed by `len` bytes.
 * If the socket is not yet connected, the frame is silently dropped.
 * Returns 0 on success (or dropped), -1 on send error (connection closed).
 */
int aa_video_output_send_frame(const uint8_t *data, size_t len);

/**
 * Tear down the output channel. Closes the socket.
 */
void aa_video_output_destroy(void);

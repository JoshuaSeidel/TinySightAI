/*
 * aa_emulator.h — AA protocol emulator for CarPlay-only mode
 *
 * The car head unit ALWAYS communicates via Android Auto Protocol (AAP).
 * When the user is running CarPlay (iPhone connected), no real Android phone
 * is providing AA session messages. This module impersonates an Android phone
 * at the AAP layer so the car continues to operate normally while we inject
 * CarPlay video wrapped as AAP channel-3 frames.
 *
 * Responsibilities:
 *
 *   1. AA version negotiation
 *      When the car sends a VERSION_REQUEST on channel 0, we respond with
 *      our supported version (major=1, minor=5).
 *
 *   2. Service discovery / capability advertisement
 *      We respond to channel-open requests and advertise a video output
 *      service so the car activates its display.
 *
 *   3. Video injection
 *      aa_emu_send_video() wraps a CarPlay H.264 NAL in an AAP video payload
 *      (channel 3, message type 0x0005, 8-byte timestamp) and sends it to
 *      the car via the car TCP socket.
 *
 *   4. Touch forwarding
 *      aa_emu_handle_car_message() parses incoming AAP frames from the car.
 *      Channel-1 (input) events are forwarded to the CarPlay touch handler
 *      via a registered callback.
 *
 *   5. Heartbeat / ping-pong
 *      AA requires periodic PING/PONG on the control channel to keep the
 *      session alive. The emulator sends PONG in response to PING.
 *
 * All AAP control messages use a minimal TLV encoding that satisfies the
 * car without requiring a full protobuf stack.
 *
 * Thread safety:
 *   aa_emu_send_video() and aa_emu_handle_car_message() may be called from
 *   different threads; internal serialisation via a mutex.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Callback invoked when the car sends a touch/input event on channel 1.
 * raw_payload/len is the AAP input channel payload (protobuf / simple TLV). */
typedef void (*aa_emu_touch_cb)(const uint8_t *raw_payload, size_t len);

/**
 * Initialise the AA emulator on an established car TCP connection.
 *
 * @param car_fd      Connected TCP socket to the T-Dongle / car head unit.
 * @param touch_cb    Callback invoked when touch events arrive from car.
 *                    May be NULL if touch forwarding is not required.
 *
 * Sends the initial AA handshake (version negotiation, service
 * advertisement) immediately.
 *
 * Returns 0 on success, -1 on error.
 */
int aa_emu_init(int car_fd, aa_emu_touch_cb touch_cb);

/**
 * Wrap H.264 NAL data in an AAP video frame and send to the car.
 *
 * @param h264     Pointer to H.264 Annex B data (keyframe or delta).
 * @param len      Number of bytes.
 * @param ts_ns    Presentation timestamp in nanoseconds (monotonic).
 *
 * Handles fragmentation automatically if len > AAP_MAX_FRAME_SIZE.
 * Returns 0 on success, -1 on send error.
 */
int aa_emu_send_video(const uint8_t *h264, size_t len, uint64_t ts_ns);

/**
 * Process an incoming AAP frame received from the car.
 *
 * Should be called for every raw frame read from the car socket.
 * Handles control channel messages internally; routes touch events to
 * the registered callback.
 *
 * @param frame   Raw bytes starting at the 6-byte AAP header.
 * @param len     Total bytes (header + payload).
 */
void aa_emu_handle_car_message(const uint8_t *frame, size_t len);

/**
 * Tear down the emulator. Closes the car socket.
 */
void aa_emu_destroy(void);

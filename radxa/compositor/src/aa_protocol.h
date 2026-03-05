#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Android Auto Protocol (AAP) frame format
 *
 * 6-byte header:
 *   byte 0:     channel_id
 *   byte 1:     flags (0x09 = first frame, 0x0A = continuation, 0x0B = last)
 *   bytes 2-3:  payload length (big-endian)
 *   bytes 4-5:  total message length (big-endian, only for first fragment)
 *
 * Channel assignments:
 *   0 = Control
 *   1 = Input (touch, keys)
 *   2 = Sensor
 *   3 = Video (H.264)
 *   4 = Media audio
 *   5 = Speech audio
 *   6 = Navigation
 *
 * Video payload (after AAP header):
 *   bytes 0-1:  message type (0x00 0x05 for MediaDataWithTimestamp)
 *   bytes 2-9:  timestamp (nanoseconds, big-endian uint64)
 *   bytes 10+:  H.264 Annex B NAL units
 */

#define AAP_HEADER_SIZE      6
#define AAP_MAX_FRAME_SIZE   16384  /* fragments > 16KB */

#define AAP_CH_CONTROL       0
#define AAP_CH_INPUT         1
#define AAP_CH_SENSOR        2
#define AAP_CH_VIDEO         3
#define AAP_CH_MEDIA_AUDIO   4
#define AAP_CH_SPEECH_AUDIO  5
#define AAP_CH_NAV           6

#define AAP_FLAG_FIRST       0x09
#define AAP_FLAG_CONT        0x0A
#define AAP_FLAG_LAST        0x0B
#define AAP_FLAG_SINGLE      0x08  /* unfragmented */

typedef struct {
    uint8_t  channel;
    uint8_t  flags;
    uint16_t payload_len;
    uint16_t total_len;
    const uint8_t *payload;
} aap_frame_t;

/**
 * Parse an AAP frame header from raw bytes.
 * Returns true if valid, populates frame struct.
 */
bool aap_parse_header(const uint8_t *data, size_t len, aap_frame_t *frame);

/**
 * Build an AAP video frame header.
 * Writes 6 bytes to `out`. Returns header size.
 */
int aap_build_video_header(uint8_t *out, uint16_t payload_len, bool fragmented);

/**
 * Extract H.264 NAL data from an AAP video payload.
 * Skips the 2-byte message type and 8-byte timestamp.
 * Returns pointer into `payload` and sets `nal_len`.
 */
const uint8_t *aap_extract_h264(const uint8_t *payload, size_t payload_len,
                                 size_t *nal_len, uint64_t *timestamp_ns);

/**
 * Build an AAP video payload with timestamp.
 * Writes message type + timestamp + H.264 data to `out`.
 * Returns total payload size.
 */
int aap_build_video_payload(uint8_t *out, size_t out_size,
                             const uint8_t *h264_data, size_t h264_len,
                             uint64_t timestamp_ns);

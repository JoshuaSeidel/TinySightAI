#pragma once

/*
 * airplay_mirror.h — AirPlay Screen Mirror Stream Parser
 *
 * The iPhone sends H.264 (or H.265 on newer iOS) video frames over a dedicated
 * TCP connection established during RTSP SETUP. Each frame is preceded by an
 * 8-byte header describing the payload type and length.
 *
 * Packet format on the wire:
 *   [4 bytes] payload length  (big-endian, NOT including this header)
 *   [2 bytes] payload type    (big-endian)
 *             0x00 = unencrypted  H.264 NAL unit(s) in Annex B format
 *             0x01 = FairPlay-encrypted NAL unit(s) (AES-128-CTR)
 *             0x02 = codec info (SPS/PPS for H.264, VPS/SPS/PPS for H.265)
 *             0x05 = heartbeat / keep-alive
 *   [2 bytes] padding (ignored)
 *   [8 bytes] timestamp (microseconds, big-endian)  -- present when type 0x00/0x01
 *   [N bytes] payload
 *
 * Total header is 16 bytes for video frames (types 0/1) and 8 bytes for others.
 *
 * FairPlay decryption:
 *   cipher  : AES-128-CTR
 *   key     : 16 bytes derived during /fp-setup handshake
 *   counter : 16-byte block where bytes [0..11] = aes_iv, bytes [12..15] = frame counter
 *             The frame counter increments once per 16-byte block.
 *
 * H.264 codec detection (first byte of NAL after the start code 0x00 0x00 0x00 0x01):
 *   0x67 = SPS (Sequence Parameter Set)
 *   0x68 = PPS (Picture Parameter Set)
 *   0x65 = IDR frame (key frame)
 *   0x41, 0x61 = Non-IDR frame (P/B frame)
 *
 * H.265 codec detection:
 *   (nal_unit_type >> 1) == 32 → VPS   (0x40)
 *   (nal_unit_type >> 1) == 33 → SPS   (0x42)
 *   (nal_unit_type >> 1) == 34 → PPS   (0x44)
 *   (nal_unit_type >> 1) == 19 → IDR   (0x26)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Mirror packet payload types */
#define MIRROR_TYPE_VIDEO_PLAIN   0x0000
#define MIRROR_TYPE_VIDEO_FP      0x0001
#define MIRROR_TYPE_CODEC_INFO    0x0002
#define MIRROR_TYPE_HEARTBEAT     0x0005

/* Header sizes */
#define MIRROR_BASE_HDR_LEN       8   /* length(4) + type(2) + pad(2) */
#define MIRROR_VIDEO_HDR_LEN      16  /* base(8) + timestamp(8) */

/* Maximum reassembly buffer (8 MB — enough for 4K IDR frames) */
#define MIRROR_MAX_FRAME_BYTES    (8 * 1024 * 1024)

/* Video codec type detected from NAL bytes */
typedef enum {
    MIRROR_CODEC_UNKNOWN = 0,
    MIRROR_CODEC_H264,
    MIRROR_CODEC_H265,
} mirror_codec_t;

/*
 * Video frame callback.
 * Called once per complete H.264/H.265 Annex-B frame.
 *   data         — pointer to the frame data (Annex B, starts with 0x00 0x00 0x00 0x01)
 *   len          — total frame length in bytes
 *   timestamp_us — presentation timestamp in microseconds
 *   codec        — H264 or H265
 *   ctx          — caller-supplied opaque pointer
 */
typedef void (*mirror_video_cb_t)(const uint8_t *data, size_t len,
                                   uint64_t timestamp_us,
                                   mirror_codec_t codec,
                                   void *ctx);

/*
 * Per-connection mirror context.
 * Allocated by the server before calling airplay_mirror_handle_connection().
 */
typedef struct {
    /* AES-128-CTR decryption state (active when fairplay_active == true) */
    bool        fairplay_active;
    uint8_t     aes_key[16];          /* derived from /fp-setup stage 3 */
    uint8_t     aes_iv[16];           /* per-stream IV from RTSP SETUP */

    /* Reassembly buffer for fragmented frames */
    uint8_t    *frame_buf;            /* dynamically allocated */
    size_t      frame_buf_cap;        /* current capacity */
    size_t      frame_buf_len;        /* bytes valid so far */

    /* Codec info (SPS/PPS or VPS/SPS/PPS) stored until next IDR */
    uint8_t     codec_info[4096];
    size_t      codec_info_len;
    mirror_codec_t codec;

    /* Callbacks */
    mirror_video_cb_t video_cb;
    void             *cb_ctx;
} airplay_mirror_ctx_t;

/*
 * Initialise a mirror context.
 * video_cb is called for each complete decoded frame.
 */
int airplay_mirror_ctx_init(airplay_mirror_ctx_t *ctx,
                             mirror_video_cb_t video_cb,
                             void *cb_ctx);

/*
 * Set FairPlay decryption parameters.
 * Called after the /fp-setup handshake has completed.
 * key must be 16 bytes; iv must be 16 bytes.
 */
void airplay_mirror_set_fairplay(airplay_mirror_ctx_t *ctx,
                                  const uint8_t key[16],
                                  const uint8_t iv[16]);

/*
 * Process a single mirror connection until EOF or error.
 * fd is the accepted TCP socket.
 * Blocks until the iPhone disconnects.
 */
int airplay_mirror_handle_connection(airplay_mirror_ctx_t *ctx, int fd);

/*
 * Free all resources held by a mirror context.
 */
void airplay_mirror_ctx_destroy(airplay_mirror_ctx_t *ctx);

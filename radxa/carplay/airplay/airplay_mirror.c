/*
 * airplay_mirror.c — AirPlay Screen Mirror Stream Parser
 *
 * Handles the TCP data connection that carries H.264/H.265 screen mirror data.
 *
 * Wire protocol (per packet):
 *   Offset  Size  Field
 *   0       4     payload_length  (big-endian, bytes that follow the 8-byte base header)
 *   4       2     payload_type    (big-endian, see MIRROR_TYPE_*)
 *   6       2     padding         (ignored)
 *   --  for video types (0x00, 0x01) only:
 *   8       8     timestamp_us    (big-endian, microseconds)
 *   --  then payload_length bytes of payload
 *
 * FairPlay decryption (AES-128-CTR):
 *   The counter block is constructed as:
 *     bytes [0..11] = aes_iv[0..11]      (12-byte fixed nonce from RTSP SETUP)
 *     bytes [12..15] = block_counter     (32-bit big-endian, starts at 0 per frame)
 *   Decryption is in-place on the payload before NAL parsing.
 */

#include "airplay_mirror.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <openssl/evp.h>

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/* Read exactly `len` bytes from fd, retrying on EINTR/short reads */
static int read_exact(int fd, void *buf, size_t len)
{
    size_t done = 0;
    uint8_t *p = (uint8_t *)buf;
    while (done < len) {
        ssize_t n = recv(fd, p + done, len - done, MSG_WAITALL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/*
 * Detect codec from first byte of a NAL unit (after the Annex-B start code).
 * H.264: nal_unit_type = byte & 0x1F
 * H.265: nal_unit_type = (byte >> 1) & 0x3F
 */
static mirror_codec_t detect_codec(const uint8_t *nal_data, size_t len)
{
    if (len < 5) return MIRROR_CODEC_UNKNOWN; /* need start code + 1 NAL byte */

    /* Skip Annex-B start code 0x00 0x00 0x00 0x01 */
    size_t off = 0;
    if (nal_data[0] == 0x00 && nal_data[1] == 0x00 &&
        nal_data[2] == 0x00 && nal_data[3] == 0x01) {
        off = 4;
    } else if (nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x01) {
        off = 3;
    }
    if (off >= len) return MIRROR_CODEC_UNKNOWN;

    uint8_t b = nal_data[off];

    /* H.265 VPS/SPS/PPS start at NAL types 32/33/34 → first byte 0x40/0x42/0x44 */
    uint8_t h265_type = (b >> 1) & 0x3F;
    if (h265_type == 32 || h265_type == 33 || h265_type == 34) {
        return MIRROR_CODEC_H265;
    }

    /* H.264 SPS (0x67) PPS (0x68) IDR (0x65) non-IDR (0x41/0x61) */
    uint8_t h264_type = b & 0x1F;
    if (h264_type >= 1 && h264_type <= 31) {
        return MIRROR_CODEC_H264;
    }

    return MIRROR_CODEC_UNKNOWN;
}

/*
 * Grow the frame reassembly buffer to hold at least `needed` bytes.
 */
static int frame_buf_ensure(airplay_mirror_ctx_t *ctx, size_t needed)
{
    if (needed > MIRROR_MAX_FRAME_BYTES) {
        fprintf(stderr, "mirror: frame too large (%zu), dropping\n", needed);
        return -1;
    }
    if (ctx->frame_buf_cap >= needed) return 0;

    size_t newcap = ctx->frame_buf_cap ? ctx->frame_buf_cap : 65536;
    while (newcap < needed) newcap *= 2;

    uint8_t *p = realloc(ctx->frame_buf, newcap);
    if (!p) {
        perror("mirror: realloc frame_buf");
        return -1;
    }
    ctx->frame_buf = p;
    ctx->frame_buf_cap = newcap;
    return 0;
}

/* -----------------------------------------------------------------------
 * FairPlay AES-128-CTR decryption (in-place)
 * ----------------------------------------------------------------------- */

/*
 * Decrypt `len` bytes of `data` in-place using AES-128-CTR.
 *
 * CTR block construction:
 *   bytes [0..11]  = iv[0..11]     (the 12-byte "nonce" from RTSP SETUP)
 *   bytes [12..15] = block_index   (big-endian uint32, 0-based per frame)
 *
 * OpenSSL's EVP_EncryptUpdate with AES-128-CTR handles the block counting
 * internally when we supply the full 16-byte IV at init time. To use our
 * custom IV layout we construct the 16-byte counter block ourselves and
 * call EVP_EncryptUpdate with ECB mode (counter mode = ECB keystream XOR).
 *
 * Simpler: use EVP_aes_128_ctr() — OpenSSL CTR mode expects a 16-byte "IV"
 * where the last 4 bytes are the initial counter value. We set them to 0.
 */
static int fairplay_decrypt(const uint8_t key[16], const uint8_t iv[16],
                             uint8_t *data, size_t len)
{
    /* Build a 16-byte IV for OpenSSL: first 12 bytes from iv, last 4 = 0 */
    uint8_t ctr_iv[16];
    memcpy(ctr_iv, iv, 12);
    ctr_iv[12] = 0; ctr_iv[13] = 0; ctr_iv[14] = 0; ctr_iv[15] = 0;

    EVP_CIPHER_CTX *cctx = EVP_CIPHER_CTX_new();
    if (!cctx) return -1;

    int ok = EVP_EncryptInit_ex(cctx, EVP_aes_128_ctr(), NULL, key, ctr_iv);
    if (ok != 1) { EVP_CIPHER_CTX_free(cctx); return -1; }

    /* In CTR mode, encrypt == decrypt */
    int out_len = 0;
    ok = EVP_EncryptUpdate(cctx, data, &out_len, data, (int)len);
    EVP_CIPHER_CTX_free(cctx);

    return (ok == 1) ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

int airplay_mirror_ctx_init(airplay_mirror_ctx_t *ctx,
                             mirror_video_cb_t video_cb,
                             void *cb_ctx)
{
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->video_cb = video_cb;
    ctx->cb_ctx   = cb_ctx;
    ctx->codec    = MIRROR_CODEC_UNKNOWN;
    return 0;
}

void airplay_mirror_set_fairplay(airplay_mirror_ctx_t *ctx,
                                  const uint8_t key[16],
                                  const uint8_t iv[16])
{
    if (!ctx) return;
    memcpy(ctx->aes_key, key, 16);
    memcpy(ctx->aes_iv,  iv,  16);
    ctx->fairplay_active = true;
    printf("mirror: FairPlay decryption enabled\n");
}

int airplay_mirror_handle_connection(airplay_mirror_ctx_t *ctx, int fd)
{
    if (!ctx || fd < 0) return -1;

    printf("mirror: stream connection opened\n");

    /*
     * Main receive loop.
     * Each iteration reads one AirPlay mirror packet.
     */
    for (;;) {
        /* ---- Read base 8-byte header ---- */
        uint8_t hdr[8];
        if (read_exact(fd, hdr, sizeof(hdr)) < 0) {
            /* EOF or disconnect — normal exit */
            break;
        }

        uint32_t payload_len = ((uint32_t)hdr[0] << 24) |
                                ((uint32_t)hdr[1] << 16) |
                                ((uint32_t)hdr[2] <<  8) |
                                 (uint32_t)hdr[3];
        uint16_t payload_type = ((uint16_t)hdr[4] << 8) | hdr[5];
        /* hdr[6], hdr[7] = padding, ignored */

        /* ---- Dispatch by type ---- */
        switch (payload_type) {

        /* ----------------------------------------------------------------
         * Heartbeat / keep-alive — no payload, nothing to do
         * ---------------------------------------------------------------- */
        case MIRROR_TYPE_HEARTBEAT:
            /* payload_len should be 0, but drain it just in case */
            if (payload_len > 0) {
                uint8_t tmp[64];
                size_t left = payload_len;
                while (left > 0) {
                    size_t chunk = left < sizeof(tmp) ? left : sizeof(tmp);
                    if (read_exact(fd, tmp, chunk) < 0) goto done;
                    left -= chunk;
                }
            }
            break;

        /* ----------------------------------------------------------------
         * Codec info packet — contains SPS/PPS (H.264) or VPS/SPS/PPS (H.265)
         * in Annex-B format. We cache it and prepend it to the next IDR.
         * ---------------------------------------------------------------- */
        case MIRROR_TYPE_CODEC_INFO: {
            /* Codec info has no timestamp prefix; payload starts immediately */
            if (payload_len == 0 || payload_len > sizeof(ctx->codec_info)) {
                fprintf(stderr, "mirror: codec info size %u out of range\n",
                        payload_len);
                /* drain and skip */
                size_t left = payload_len;
                uint8_t tmp[256];
                while (left > 0) {
                    size_t chunk = left < sizeof(tmp) ? left : sizeof(tmp);
                    if (read_exact(fd, tmp, chunk) < 0) goto done;
                    left -= chunk;
                }
                break;
            }
            if (read_exact(fd, ctx->codec_info, payload_len) < 0) goto done;
            ctx->codec_info_len = payload_len;
            ctx->codec = detect_codec(ctx->codec_info, ctx->codec_info_len);
            printf("mirror: codec info received (%zu bytes, codec=%s)\n",
                   ctx->codec_info_len,
                   ctx->codec == MIRROR_CODEC_H265 ? "H.265" : "H.264");
            break;
        }

        /* ----------------------------------------------------------------
         * Video frame — plaintext (0x00) or FairPlay-encrypted (0x01)
         * Both have a 8-byte timestamp following the base header.
         * ---------------------------------------------------------------- */
        case MIRROR_TYPE_VIDEO_PLAIN:
        case MIRROR_TYPE_VIDEO_FP: {
            if (payload_len < 8) {
                /* Malformed: need at least timestamp */
                fprintf(stderr, "mirror: video packet too short (%u)\n",
                        payload_len);
                goto done;
            }

            /* Read timestamp (8 bytes, microseconds) */
            uint8_t ts_buf[8];
            if (read_exact(fd, ts_buf, sizeof(ts_buf)) < 0) goto done;
            uint64_t timestamp_us =
                ((uint64_t)ts_buf[0] << 56) | ((uint64_t)ts_buf[1] << 48) |
                ((uint64_t)ts_buf[2] << 40) | ((uint64_t)ts_buf[3] << 32) |
                ((uint64_t)ts_buf[4] << 24) | ((uint64_t)ts_buf[5] << 16) |
                ((uint64_t)ts_buf[6] <<  8) |  (uint64_t)ts_buf[7];

            /* Actual video data length = payload_len - 8 (we consumed the ts) */
            size_t data_len = payload_len - 8;
            if (data_len == 0) break; /* empty frame, skip */

            /* Ensure reassembly buffer is large enough */
            if (frame_buf_ensure(ctx, data_len) < 0) {
                /* Skip this frame */
                size_t left = data_len;
                uint8_t tmp[512];
                while (left > 0) {
                    size_t chunk = left < sizeof(tmp) ? left : sizeof(tmp);
                    if (read_exact(fd, tmp, chunk) < 0) goto done;
                    left -= chunk;
                }
                break;
            }

            /* Read video payload */
            if (read_exact(fd, ctx->frame_buf, data_len) < 0) goto done;

            /* Decrypt if FairPlay-encrypted */
            if (payload_type == MIRROR_TYPE_VIDEO_FP) {
                if (!ctx->fairplay_active) {
                    fprintf(stderr, "mirror: encrypted frame but FairPlay not set up\n");
                    break;
                }
                if (fairplay_decrypt(ctx->aes_key, ctx->aes_iv,
                                      ctx->frame_buf, data_len) < 0) {
                    fprintf(stderr, "mirror: AES-CTR decryption failed\n");
                    break;
                }
            }

            /* Detect codec if not yet known */
            if (ctx->codec == MIRROR_CODEC_UNKNOWN) {
                ctx->codec = detect_codec(ctx->frame_buf, data_len);
            }

            /* Deliver to callback */
            if (ctx->video_cb) {
                ctx->video_cb(ctx->frame_buf, data_len,
                               timestamp_us, ctx->codec, ctx->cb_ctx);
            }
            break;
        }

        default:
            /* Unknown type — drain payload and continue */
            fprintf(stderr, "mirror: unknown packet type 0x%04x (%u bytes), skipping\n",
                    payload_type, payload_len);
            {
                size_t left = payload_len;
                uint8_t tmp[512];
                while (left > 0) {
                    size_t chunk = left < sizeof(tmp) ? left : sizeof(tmp);
                    if (read_exact(fd, tmp, chunk) < 0) goto done;
                    left -= chunk;
                }
            }
            break;
        } /* switch */
    } /* for */

done:
    printf("mirror: stream connection closed\n");
    return 0;
}

void airplay_mirror_ctx_destroy(airplay_mirror_ctx_t *ctx)
{
    if (!ctx) return;
    free(ctx->frame_buf);
    ctx->frame_buf     = NULL;
    ctx->frame_buf_cap = 0;
    ctx->frame_buf_len = 0;
    /* Clear key material */
    memset(ctx->aes_key, 0, sizeof(ctx->aes_key));
    memset(ctx->aes_iv,  0, sizeof(ctx->aes_iv));
}

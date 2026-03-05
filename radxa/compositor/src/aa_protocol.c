#include "aa_protocol.h"
#include <string.h>
#include <arpa/inet.h>

bool aap_parse_header(const uint8_t *data, size_t len, aap_frame_t *frame)
{
    if (!data || len < AAP_HEADER_SIZE || !frame)
        return false;

    frame->channel     = data[0];
    frame->flags       = data[1];
    frame->payload_len = (uint16_t)(data[2] << 8 | data[3]);
    frame->total_len   = (uint16_t)(data[4] << 8 | data[5]);
    frame->payload     = data + AAP_HEADER_SIZE;

    /* Sanity check */
    if (frame->payload_len > len - AAP_HEADER_SIZE)
        return false;

    return true;
}

int aap_build_video_header(uint8_t *out, uint16_t payload_len, bool fragmented)
{
    out[0] = AAP_CH_VIDEO;
    out[1] = fragmented ? AAP_FLAG_FIRST : AAP_FLAG_SINGLE;
    out[2] = (uint8_t)(payload_len >> 8);
    out[3] = (uint8_t)(payload_len & 0xFF);
    out[4] = out[2]; /* total = payload for single frame */
    out[5] = out[3];
    return AAP_HEADER_SIZE;
}

const uint8_t *aap_extract_h264(const uint8_t *payload, size_t payload_len,
                                 size_t *nal_len, uint64_t *timestamp_ns)
{
    if (payload_len < 10)
        return NULL;

    /* Skip 2-byte message type */
    /* Read 8-byte timestamp (big-endian) */
    *timestamp_ns = 0;
    for (int i = 0; i < 8; i++) {
        *timestamp_ns = (*timestamp_ns << 8) | payload[2 + i];
    }

    *nal_len = payload_len - 10;
    return payload + 10;
}

int aap_build_video_payload(uint8_t *out, size_t out_size,
                             const uint8_t *h264_data, size_t h264_len,
                             uint64_t timestamp_ns)
{
    size_t needed = 10 + h264_len;
    if (needed > out_size)
        return -1;

    /* Message type: MediaDataWithTimestamp = 0x0005 */
    out[0] = 0x00;
    out[1] = 0x05;

    /* Timestamp (big-endian uint64) */
    for (int i = 7; i >= 0; i--) {
        out[2 + (7 - i)] = (uint8_t)(timestamp_ns >> (i * 8));
    }

    memcpy(out + 10, h264_data, h264_len);
    return (int)needed;
}

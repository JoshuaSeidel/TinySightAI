/*
 * nal_detect.c — H.264/H.265 NAL unit auto-detection
 *
 * Operates on Annex B bitstreams (start-code delimited).
 * No dynamic allocation; all scanning is done in-place.
 */
#include "nal_detect.h"

#include <string.h>

/* ---- Start code scanning ---- */

/*
 * Returns the length of the start code (3 or 4) at data[pos], or 0 if
 * data[pos..pos+2] is not 0x00 0x00 0x01.
 */
static int sc_len_at(const uint8_t *data, size_t len, size_t pos)
{
    if (pos + 2 >= len)
        return 0;
    if (data[pos] != 0x00 || data[pos + 1] != 0x00 || data[pos + 2] != 0x01)
        return 0;
    /* Prefer 4-byte form when preceded by 0x00 */
    if (pos > 0 && data[pos - 1] == 0x00)
        return 4; /* caller sees the lead zero as part of the start code */
    return 3;
}

int nal_find_start_code(const uint8_t *data, size_t len, size_t *offset)
{
    if (!data || !offset || len < 3)
        return 0;

    for (size_t i = 0; i + 2 < len; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            /* Determine whether this is 3- or 4-byte start code.
             * We report the position of the 0x00 0x00 0x01 triplet so that
             * callers can read the NAL header byte at *offset + 3. */
            int sclen;
            if (i >= 1 && data[i - 1] == 0x00) {
                *offset = i - 1; /* include the leading zero byte */
                sclen = 4;
            } else {
                *offset = i;
                sclen = 3;
            }
            return sclen;
        }
    }
    return 0;
}

/* ---- NAL header extraction ---- */

/*
 * Returns a pointer to the first NAL header byte after a start code, or NULL.
 * Also fills *sc_sz with the start-code length (3 or 4).
 */
static const uint8_t *first_nal_header(const uint8_t *data, size_t len,
                                        int *sc_sz)
{
    size_t offset = 0;
    int sclen = nal_find_start_code(data, len, &offset);
    if (sclen == 0)
        return NULL;

    size_t nal_pos = offset + sclen;
    if (nal_pos >= len)
        return NULL;

    if (sc_sz)
        *sc_sz = sclen;
    return data + nal_pos;
}

/* ---- Codec detection ---- */

/*
 * H.264 NAL types (bits[4:0] of first NAL header byte):
 *   1  = Non-IDR slice
 *   5  = IDR slice
 *   6  = SEI
 *   7  = SPS  ← strong indicator
 *   8  = PPS  ← strong indicator
 *   9  = Access unit delimiter
 *
 * H.265 NAL types (bits[14:9] of the two-byte NAL header, i.e.
 *   (byte0 >> 1) & 0x3F):
 *   1  = TRAIL_R (non-IDR)
 *   19 = IDR_W_RADL
 *   20 = IDR_N_LP
 *   32 = VPS  ← strong indicator
 *   33 = SPS  ← strong indicator
 *   34 = PPS  ← strong indicator
 *   35 = ACCESS_UNIT_DELIMITER
 *
 * Strategy: walk all NAL units in the buffer. Award points for H.264 vs
 * H.265 indicator NAL types. Return the winner; ties → H.264.
 */
int nal_detect_codec(const uint8_t *data, size_t len)
{
    if (!data || len < 4)
        return -1;

    int score_h264 = 0;
    int score_h265 = 0;
    size_t pos = 0;

    while (pos < len) {
        size_t offset = pos;
        int sclen = nal_find_start_code(data + pos, len - pos, &offset);
        if (sclen == 0)
            break;

        /* Absolute position of start code within data[] */
        size_t sc_abs = pos + offset;
        size_t nal_abs = sc_abs + sclen;

        if (nal_abs >= len)
            break;

        uint8_t nal_byte = data[nal_abs];

        /* H.264 NAL type */
        int h264_type = nal_byte & 0x1F;

        /* H.265 requires two header bytes; check bounds */
        int h265_type = -1;
        if (nal_abs + 1 < len) {
            /* H.265: forbidden_zero_bit[15] | nal_unit_type[14:9] | ... */
            h265_type = (nal_byte >> 1) & 0x3F;
        }

        /* Score strong H.264 indicators */
        if (h264_type == 7 || h264_type == 8) score_h264 += 3;     /* SPS/PPS */
        else if (h264_type == 5 || h264_type == 1) score_h264 += 1; /* IDR/non-IDR */
        else if (h264_type == 9 || h264_type == 6) score_h264 += 1; /* AUD/SEI */

        /* Score strong H.265 indicators */
        if (h265_type >= 0) {
            if (h265_type == 32 || h265_type == 33 || h265_type == 34)
                score_h265 += 3; /* VPS/SPS/PPS */
            else if (h265_type == 19 || h265_type == 20)
                score_h265 += 1; /* IDR */
            else if (h265_type == 1 || h265_type == 35)
                score_h265 += 1; /* TRAIL_R / AUD */
        }

        /* Advance past this start code to scan further NAL units */
        pos = nal_abs + 1;
    }

    if (score_h264 == 0 && score_h265 == 0)
        return -1;

    return (score_h265 > score_h264) ? CODEC_H265 : CODEC_H264;
}

/* ---- Keyframe detection ---- */

int nal_is_keyframe(const uint8_t *data, size_t len, input_codec_t codec)
{
    if (!data || len < 4)
        return -1;

    size_t pos = 0;
    while (pos < len) {
        size_t offset = pos;
        int sclen = nal_find_start_code(data + pos, len - pos, &offset);
        if (sclen == 0)
            break;

        size_t sc_abs = pos + offset;
        size_t nal_abs = sc_abs + sclen;

        if (nal_abs >= len)
            break;

        uint8_t nal_byte = data[nal_abs];

        if (codec == CODEC_H264) {
            int nal_type = nal_byte & 0x1F;
            if (nal_type == 5) /* IDR */
                return 1;
        } else { /* CODEC_H265 */
            int nal_type = (nal_byte >> 1) & 0x3F;
            if (nal_type == 19 || nal_type == 20) /* IDR_W_RADL / IDR_N_LP */
                return 1;
        }

        pos = nal_abs + 1;
    }

    return 0;
}

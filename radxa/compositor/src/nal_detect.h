/*
 * nal_detect.h — H.264/H.265 NAL unit auto-detection
 *
 * Scans Annex B bitstreams for start codes and classifies NAL unit type
 * and codec type. Used by the CarPlay input path and AAP video intercept
 * to avoid hardcoding assumptions about which codec the phone sends.
 *
 * Start code formats recognized:
 *   3-byte: 0x00 0x00 0x01
 *   4-byte: 0x00 0x00 0x00 0x01  (preferred, used for SPS/PPS)
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "pipeline.h"   /* input_codec_t, CODEC_H264, CODEC_H265 */

/**
 * Detect codec from the first NAL unit found in data.
 *
 * Heuristic:
 *   H.264 NAL types live in bits [4:0] of the NAL header byte.
 *   H.265 NAL types live in bits [14:9] of the two-byte NAL header.
 *   We check for well-known parameter set NAL types first:
 *     H.264: type 7 (SPS), 8 (PPS)
 *     H.265: type 32 (VPS), 33 (SPS), 34 (PPS)
 *   If none found, fall back to checking IDR/non-IDR types.
 *
 * Returns CODEC_H264, CODEC_H265, or -1 if undetermined.
 */
int nal_detect_codec(const uint8_t *data, size_t len);

/**
 * Check whether a frame contains at least one IDR (keyframe) NAL unit.
 *
 * H.264 IDR: nal_type == 5
 * H.265 IDR: nal_type == 19 (IDR_W_RADL) or 20 (IDR_N_LP)
 *
 * Returns 1 if IDR found, 0 if not, -1 on error.
 */
int nal_is_keyframe(const uint8_t *data, size_t len, input_codec_t codec);

/**
 * Find the next Annex B start code (3- or 4-byte) in data[offset..len).
 *
 * On success, sets *offset to the position of the 0x00 0x00 0x01 bytes
 * (i.e. the start of the 3-byte form; subtract 1 for the 4-byte form when
 * data[offset-1] == 0x00) and returns the start-code length (3 or 4).
 *
 * Returns 0 if no start code found.
 */
int nal_find_start_code(const uint8_t *data, size_t len, size_t *offset);

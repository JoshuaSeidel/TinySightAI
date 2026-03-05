#!/usr/bin/env python3
"""
test_nal_detection.py — H.264 and H.265 NAL Unit Detection Tests

Tests NAL unit type detection and keyframe identification logic
matching what the compositor uses when processing video streams.

H.264 NAL unit types (nal_unit_type = first byte after Annex-B start code,
masked with 0x1F):
    1   Non-IDR (P/B frame slice)
    5   IDR slice  (keyframe)
    7   SPS        (Sequence Parameter Set)
    8   PPS        (Picture Parameter Set)
    6   SEI        (Supplemental Enhancement Information)
    9   AUD        (Access Unit Delimiter)

H.264 byte values after start code as used in practice:
    0x67 = 0110 0111 → nal_ref_idc=3, nal_unit_type=7 (SPS)
    0x68 = 0110 1000 → nal_ref_idc=3, nal_unit_type=8 (PPS)
    0x65 = 0110 0101 → nal_ref_idc=3, nal_unit_type=5 (IDR)
    0x61 = 0110 0001 → nal_ref_idc=3, nal_unit_type=1 (non-IDR)
    0x41 = 0100 0001 → nal_ref_idc=2, nal_unit_type=1 (non-IDR)

H.265 NAL unit type: bits [9:15] of the two-byte NAL header.
    nal_unit_type = (header_byte0 >> 1) & 0x3F

H.265 types:
    19  IDR_W_RADL  (keyframe)
    20  IDR_N_LP    (keyframe)
    32  VPS         (Video Parameter Set)
    33  SPS
    34  PPS
"""

import struct
import unittest

# ---------------------------------------------------------------------------
# Annex-B start codes
# ---------------------------------------------------------------------------

START_CODE_4 = b"\x00\x00\x00\x01"
START_CODE_3 = b"\x00\x00\x01"

# ---------------------------------------------------------------------------
# H.264 NAL unit type constants
# ---------------------------------------------------------------------------

H264_NAL_NON_IDR = 1
H264_NAL_IDR     = 5
H264_NAL_SEI     = 6
H264_NAL_SPS     = 7
H264_NAL_PPS     = 8
H264_NAL_AUD     = 9

# H.264 byte values as they appear in practice
H264_BYTE_SPS     = 0x67
H264_BYTE_PPS     = 0x68
H264_BYTE_IDR_1   = 0x65   # nal_ref_idc=3
H264_BYTE_IDR_2   = 0x25   # nal_ref_idc=1 (less common)
H264_BYTE_NONIDR_HI = 0x61  # nal_ref_idc=3
H264_BYTE_NONIDR_LO = 0x41  # nal_ref_idc=2

# ---------------------------------------------------------------------------
# H.265 NAL unit type constants
# ---------------------------------------------------------------------------

H265_NAL_IDR_W_RADL = 19
H265_NAL_IDR_N_LP   = 20
H265_NAL_VPS        = 32
H265_NAL_SPS        = 33
H265_NAL_PPS        = 34

# ---------------------------------------------------------------------------
# Detection functions
# (These mirror what the compositor / airplay_mirror.c implements)
# ---------------------------------------------------------------------------


def annex_b_find_start_code(data: bytes) -> int:
    """
    Find the first Annex-B start code (4-byte or 3-byte) in data.
    Returns the offset of the start of the start code, or -1 if not found.
    """
    i = 0
    while i < len(data) - 3:
        if data[i:i+4] == START_CODE_4:
            return i
        if data[i:i+3] == START_CODE_3:
            return i
        i += 1
    return -1


def h264_nal_type(nal_byte: int) -> int:
    """Extract H.264 NAL unit type from the first NAL byte."""
    return nal_byte & 0x1F


def h264_is_keyframe(nal_byte: int) -> bool:
    """Return True if the NAL byte indicates an IDR (keyframe)."""
    return h264_nal_type(nal_byte) == H264_NAL_IDR


def h264_is_sps(nal_byte: int) -> bool:
    return h264_nal_type(nal_byte) == H264_NAL_SPS


def h264_is_pps(nal_byte: int) -> bool:
    return h264_nal_type(nal_byte) == H264_NAL_PPS


def h265_nal_type(header_byte0: int) -> int:
    """
    Extract H.265 NAL unit type from the first byte of the 2-byte NAL header.
    nal_unit_type = (header_byte0 >> 1) & 0x3F
    """
    return (header_byte0 >> 1) & 0x3F


def h265_is_keyframe(header_byte0: int) -> bool:
    """Return True if this is an H.265 IDR NAL (IDR_W_RADL or IDR_N_LP)."""
    nt = h265_nal_type(header_byte0)
    return nt in (H265_NAL_IDR_W_RADL, H265_NAL_IDR_N_LP)


def h265_is_vps(header_byte0: int) -> bool:
    return h265_nal_type(header_byte0) == H265_NAL_VPS


def h265_is_sps(header_byte0: int) -> bool:
    return h265_nal_type(header_byte0) == H265_NAL_SPS


def h265_is_pps(header_byte0: int) -> bool:
    return h265_nal_type(header_byte0) == H265_NAL_PPS


def detect_codec_from_nal(data: bytes) -> str:
    """
    Heuristically detect whether data is H.264 or H.265 Annex-B.

    Strategy:
      - Find first start code.
      - Look at NAL byte(s) after it.
      - H.264 SPS byte is 0x67 (very common leading NAL in codec params).
      - H.265 VPS/SPS header bytes have bit patterns incompatible with H.264.

    Returns "h264", "h265", or "unknown".
    """
    sc_offset = annex_b_find_start_code(data)
    if sc_offset < 0:
        return "unknown"

    # Advance past start code
    if data[sc_offset:sc_offset+4] == START_CODE_4:
        nal_offset = sc_offset + 4
    else:
        nal_offset = sc_offset + 3

    if nal_offset >= len(data):
        return "unknown"

    nal_byte = data[nal_offset]
    nal_type_264 = h264_nal_type(nal_byte)
    nal_type_265 = h265_nal_type(nal_byte)

    # H.264 SPS/PPS are very distinctive
    if nal_type_264 in (H264_NAL_SPS, H264_NAL_PPS,
                         H264_NAL_IDR, H264_NAL_NON_IDR,
                         H264_NAL_SEI, H264_NAL_AUD):
        # Check if it could also be H.265
        if nal_type_265 in (H265_NAL_VPS, H265_NAL_SPS, H265_NAL_PPS):
            # Ambiguous — need more context; prefer H.264 as more common
            return "h264"
        return "h264"

    if nal_type_265 in (H265_NAL_VPS, H265_NAL_SPS, H265_NAL_PPS,
                         H265_NAL_IDR_W_RADL, H265_NAL_IDR_N_LP):
        return "h265"

    return "unknown"


def find_all_nal_units(data: bytes) -> list:
    """
    Split Annex-B stream into a list of (offset, nal_byte) tuples,
    one per NAL unit.
    """
    nals = []
    i = 0
    while i < len(data):
        sc_len = 0
        if data[i:i+4] == START_CODE_4:
            sc_len = 4
        elif data[i:i+3] == START_CODE_3:
            sc_len = 3
        else:
            i += 1
            continue

        nal_start = i + sc_len
        if nal_start < len(data):
            nals.append((nal_start, data[nal_start]))
        i = nal_start + 1

    return nals


# ---------------------------------------------------------------------------
# Test data factories
# ---------------------------------------------------------------------------

def make_h264_sps_pps_idr() -> bytes:
    """Build a minimal H.264 Annex-B stream: SPS + PPS + IDR."""
    sps = START_CODE_4 + bytes([H264_BYTE_SPS]) + b"\x42\x00\x1e" + b"\xda\x01\xe0"
    pps = START_CODE_4 + bytes([H264_BYTE_PPS]) + b"\xce\x38\x80"
    idr = START_CODE_4 + bytes([H264_BYTE_IDR_1]) + b"\xb8\x00\x04\x00\x00"
    return sps + pps + idr


def make_h264_p_frame() -> bytes:
    return START_CODE_4 + bytes([H264_BYTE_NONIDR_HI]) + b"\x9a\x04\x00\x00"


def make_h265_vps_sps_pps_idr() -> bytes:
    """Build minimal H.265 Annex-B stream: VPS + SPS + PPS + IDR."""
    # H.265 NAL header: 2 bytes
    # First byte: (nal_unit_type << 1) | forbidden_zero_bit
    def h265_hdr(nal_type: int) -> bytes:
        byte0 = (nal_type << 1) & 0xFF
        byte1 = 0x01   # nuh_layer_id=0, nuh_temporal_id_plus1=1
        return bytes([byte0, byte1])

    vps = START_CODE_4 + h265_hdr(H265_NAL_VPS) + b"\x00" * 4
    sps = START_CODE_4 + h265_hdr(H265_NAL_SPS) + b"\x00" * 4
    pps = START_CODE_4 + h265_hdr(H265_NAL_PPS) + b"\x00" * 4
    idr = START_CODE_4 + h265_hdr(H265_NAL_IDR_W_RADL) + b"\x00" * 8
    return vps + sps + pps + idr


# ---------------------------------------------------------------------------
# Tests — H.264 NAL detection
# ---------------------------------------------------------------------------

class TestH264NALDetection(unittest.TestCase):

    def test_sps_byte(self):
        self.assertEqual(h264_nal_type(H264_BYTE_SPS), H264_NAL_SPS)
        self.assertTrue(h264_is_sps(H264_BYTE_SPS))
        self.assertFalse(h264_is_keyframe(H264_BYTE_SPS))

    def test_pps_byte(self):
        self.assertEqual(h264_nal_type(H264_BYTE_PPS), H264_NAL_PPS)
        self.assertTrue(h264_is_pps(H264_BYTE_PPS))
        self.assertFalse(h264_is_keyframe(H264_BYTE_PPS))

    def test_idr_byte_1(self):
        self.assertEqual(h264_nal_type(H264_BYTE_IDR_1), H264_NAL_IDR)
        self.assertTrue(h264_is_keyframe(H264_BYTE_IDR_1))
        self.assertFalse(h264_is_sps(H264_BYTE_IDR_1))

    def test_idr_byte_2(self):
        # 0x25 = 0010 0101 → nal_unit_type = 5 (IDR)
        self.assertEqual(h264_nal_type(H264_BYTE_IDR_2), H264_NAL_IDR)
        self.assertTrue(h264_is_keyframe(H264_BYTE_IDR_2))

    def test_nonidr_high(self):
        self.assertEqual(h264_nal_type(H264_BYTE_NONIDR_HI), H264_NAL_NON_IDR)
        self.assertFalse(h264_is_keyframe(H264_BYTE_NONIDR_HI))

    def test_nonidr_low(self):
        self.assertEqual(h264_nal_type(H264_BYTE_NONIDR_LO), H264_NAL_NON_IDR)
        self.assertFalse(h264_is_keyframe(H264_BYTE_NONIDR_LO))

    def test_nal_type_mask(self):
        """NAL type is only the lower 5 bits."""
        for byte_val in range(256):
            nt = h264_nal_type(byte_val)
            self.assertGreaterEqual(nt, 0)
            self.assertLessEqual(nt, 31)


class TestH264StartCode(unittest.TestCase):

    def test_find_4byte_start_code(self):
        data = b"\x00\x00\x00\x00\x00\x00\x01" + bytes([H264_BYTE_SPS]) + b"\x42"
        # Only 4-byte start code at offset 3
        data = b"\x00\x00\x00" + START_CODE_4 + bytes([H264_BYTE_SPS])
        offset = annex_b_find_start_code(data)
        self.assertEqual(offset, 3)

    def test_find_start_code_at_beginning(self):
        data = START_CODE_4 + bytes([H264_BYTE_SPS])
        self.assertEqual(annex_b_find_start_code(data), 0)

    def test_no_start_code(self):
        data = b"\x01\x02\x03\x04\x05"
        self.assertEqual(annex_b_find_start_code(data), -1)

    def test_empty_data(self):
        self.assertEqual(annex_b_find_start_code(b""), -1)


class TestH264StreamParsing(unittest.TestCase):

    def test_sps_pps_idr_stream(self):
        stream = make_h264_sps_pps_idr()
        nals = find_all_nal_units(stream)

        self.assertEqual(len(nals), 3)
        _, nal0 = nals[0]
        _, nal1 = nals[1]
        _, nal2 = nals[2]

        self.assertTrue(h264_is_sps(nal0), f"Expected SPS, got 0x{nal0:02X}")
        self.assertTrue(h264_is_pps(nal1), f"Expected PPS, got 0x{nal1:02X}")
        self.assertTrue(h264_is_keyframe(nal2), f"Expected IDR, got 0x{nal2:02X}")

    def test_p_frame_stream(self):
        stream = make_h264_p_frame()
        nals = find_all_nal_units(stream)
        self.assertGreater(len(nals), 0)
        _, nal0 = nals[0]
        self.assertFalse(h264_is_keyframe(nal0))
        self.assertEqual(h264_nal_type(nal0), H264_NAL_NON_IDR)


# ---------------------------------------------------------------------------
# Tests — H.265 NAL detection
# ---------------------------------------------------------------------------

class TestH265NALDetection(unittest.TestCase):

    def _make_h265_byte(self, nal_type: int) -> int:
        """First byte of 2-byte H.265 NAL header for the given type."""
        return (nal_type << 1) & 0xFF

    def test_vps(self):
        byte0 = self._make_h265_byte(H265_NAL_VPS)
        self.assertEqual(h265_nal_type(byte0), H265_NAL_VPS)
        self.assertTrue(h265_is_vps(byte0))
        self.assertFalse(h265_is_keyframe(byte0))

    def test_sps(self):
        byte0 = self._make_h265_byte(H265_NAL_SPS)
        self.assertEqual(h265_nal_type(byte0), H265_NAL_SPS)
        self.assertTrue(h265_is_sps(byte0))
        self.assertFalse(h265_is_keyframe(byte0))

    def test_pps(self):
        byte0 = self._make_h265_byte(H265_NAL_PPS)
        self.assertEqual(h265_nal_type(byte0), H265_NAL_PPS)
        self.assertTrue(h265_is_pps(byte0))
        self.assertFalse(h265_is_keyframe(byte0))

    def test_idr_w_radl(self):
        byte0 = self._make_h265_byte(H265_NAL_IDR_W_RADL)
        self.assertEqual(h265_nal_type(byte0), H265_NAL_IDR_W_RADL)
        self.assertTrue(h265_is_keyframe(byte0))

    def test_idr_n_lp(self):
        byte0 = self._make_h265_byte(H265_NAL_IDR_N_LP)
        self.assertEqual(h265_nal_type(byte0), H265_NAL_IDR_N_LP)
        self.assertTrue(h265_is_keyframe(byte0))

    def test_nal_type_range(self):
        """H.265 NAL type is 6 bits (0-63)."""
        for nt in range(64):
            byte0 = (nt << 1) & 0xFF
            result = h265_nal_type(byte0)
            self.assertEqual(result, nt)


class TestH265StreamParsing(unittest.TestCase):

    def test_vps_sps_pps_idr_stream(self):
        stream = make_h265_vps_sps_pps_idr()
        nals = find_all_nal_units(stream)

        self.assertEqual(len(nals), 4)
        _, b0 = nals[0]
        _, b1 = nals[1]
        _, b2 = nals[2]
        _, b3 = nals[3]

        self.assertTrue(h265_is_vps(b0), f"Expected VPS, got type {h265_nal_type(b0)}")
        self.assertTrue(h265_is_sps(b1), f"Expected SPS, got type {h265_nal_type(b1)}")
        self.assertTrue(h265_is_pps(b2), f"Expected PPS, got type {h265_nal_type(b2)}")
        self.assertTrue(h265_is_keyframe(b3), f"Expected IDR, got type {h265_nal_type(b3)}")


# ---------------------------------------------------------------------------
# Tests — Codec detection
# ---------------------------------------------------------------------------

class TestCodecDetection(unittest.TestCase):

    def test_detect_h264_from_sps_first(self):
        stream = make_h264_sps_pps_idr()
        codec = detect_codec_from_nal(stream)
        self.assertEqual(codec, "h264")

    def test_detect_h264_from_p_frame(self):
        stream = make_h264_p_frame()
        codec = detect_codec_from_nal(stream)
        self.assertEqual(codec, "h264")

    def test_detect_h265_from_vps(self):
        stream = make_h265_vps_sps_pps_idr()
        codec = detect_codec_from_nal(stream)
        self.assertEqual(codec, "h265")

    def test_detect_unknown_no_start_code(self):
        codec = detect_codec_from_nal(b"\x01\x02\x03\x04\x05")
        self.assertEqual(codec, "unknown")

    def test_detect_unknown_empty(self):
        codec = detect_codec_from_nal(b"")
        self.assertEqual(codec, "unknown")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)

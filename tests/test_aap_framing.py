#!/usr/bin/env python3
"""
test_aap_framing.py — AAP (Android Auto Protocol) Frame Parsing and Building

Tests:
    - Build frames with known channel/flags/payload
    - Parse built frames back and verify fields
    - Fragment splitting for large payloads
    - Fragment reassembly into complete messages

AAP Frame Format:
    [2 bytes] length     — total frame length including header (big-endian)
    [1 byte]  channel    — channel number
    [1 byte]  flags      — frame flags (fragment/first/last bits)
    [N bytes] payload    — message data

Flags byte:
    0x01  FIRST_FRAME  — first fragment of a multi-frame message
    0x02  LAST_FRAME   — last fragment of a multi-frame message
    0x04  ENCRYPTED    — payload is encrypted
    0x08  CONTROL      — control frame (not a data message)

    Single-frame messages set both FIRST_FRAME | LAST_FRAME (0x03).

Channels:
    0  — Control
    1  — Input (touch, sensor)
    3  — Video (H.264 Annex B)
    4  — Media audio
    7  — Voice audio
"""

import struct
import socket
import unittest
from typing import List, Tuple

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

AAP_CHANNEL_CONTROL = 0
AAP_CHANNEL_INPUT   = 1
AAP_CHANNEL_VIDEO   = 3
AAP_CHANNEL_MEDIA   = 4
AAP_CHANNEL_VOICE   = 7

AAP_FLAG_FIRST   = 0x01
AAP_FLAG_LAST    = 0x02
AAP_FLAG_ENC     = 0x04
AAP_FLAG_CONTROL = 0x08

AAP_FLAG_SINGLE  = AAP_FLAG_FIRST | AAP_FLAG_LAST  # 0x03

AAP_HEADER_SIZE  = 4  # 2-byte length + 1-byte channel + 1-byte flags

# Maximum payload per frame (chosen small to exercise fragmentation in tests)
TEST_FRAGMENT_SIZE = 64

# ---------------------------------------------------------------------------
# Frame builder / parser
# ---------------------------------------------------------------------------

def build_frame(channel: int, flags: int, payload: bytes) -> bytes:
    """
    Build a single AAP frame.

    Returns bytes: [length(2)] [channel(1)] [flags(1)] [payload(N)]
    """
    total_len = AAP_HEADER_SIZE + len(payload)
    header = struct.pack(">HBB", total_len, channel & 0xFF, flags & 0xFF)
    return header + payload


def parse_frame(data: bytes) -> Tuple[int, int, bytes]:
    """
    Parse a single AAP frame from a byte buffer.

    Returns (channel, flags, payload) on success.
    Raises ValueError if the buffer is too short or length field is invalid.
    """
    if len(data) < AAP_HEADER_SIZE:
        raise ValueError(
            f"Buffer too short: {len(data)} < {AAP_HEADER_SIZE}"
        )

    total_len, channel, flags = struct.unpack_from(">HBB", data, 0)

    if total_len < AAP_HEADER_SIZE:
        raise ValueError(
            f"Frame length field {total_len} < minimum {AAP_HEADER_SIZE}"
        )

    expected_payload_len = total_len - AAP_HEADER_SIZE

    if len(data) < total_len:
        raise ValueError(
            f"Buffer truncated: have {len(data)}, need {total_len}"
        )

    payload = data[AAP_HEADER_SIZE: total_len]

    if len(payload) != expected_payload_len:
        raise ValueError(
            f"Payload length mismatch: got {len(payload)}, "
            f"expected {expected_payload_len}"
        )

    return channel, flags, payload


def split_into_fragments(channel: int, message: bytes,
                          max_payload: int = TEST_FRAGMENT_SIZE) -> List[bytes]:
    """
    Split a large message into a list of AAP frames with proper
    FIRST/LAST flags.

    Single-frame messages: flags = AAP_FLAG_SINGLE
    Multi-frame messages:
        first frame:  AAP_FLAG_FIRST
        middle frames: 0x00
        last frame:   AAP_FLAG_LAST
    """
    if len(message) == 0:
        return [build_frame(channel, AAP_FLAG_SINGLE, b"")]

    chunks = [
        message[i: i + max_payload]
        for i in range(0, len(message), max_payload)
    ]

    if len(chunks) == 1:
        return [build_frame(channel, AAP_FLAG_SINGLE, chunks[0])]

    frames = []
    for idx, chunk in enumerate(chunks):
        if idx == 0:
            flags = AAP_FLAG_FIRST
        elif idx == len(chunks) - 1:
            flags = AAP_FLAG_LAST
        else:
            flags = 0x00
        frames.append(build_frame(channel, flags, chunk))

    return frames


def reassemble_fragments(frames: List[bytes]) -> Tuple[int, bytes]:
    """
    Reassemble a list of raw frame byte strings into (channel, message).

    Validates that FIRST/LAST flags form a consistent sequence.
    Raises ValueError on protocol errors.
    """
    if not frames:
        raise ValueError("Empty frame list")

    payload_parts = []
    first_channel = None

    for idx, raw in enumerate(frames):
        channel, flags, payload = parse_frame(raw)

        if first_channel is None:
            first_channel = channel
        elif channel != first_channel:
            raise ValueError(
                f"Channel mismatch in fragment {idx}: "
                f"expected {first_channel}, got {channel}"
            )

        if idx == 0 and not (flags & AAP_FLAG_FIRST):
            raise ValueError("First frame missing FIRST_FRAME flag")

        if idx == len(frames) - 1 and not (flags & AAP_FLAG_LAST):
            raise ValueError("Last frame missing LAST_FRAME flag")

        if 0 < idx < len(frames) - 1:
            if flags & (AAP_FLAG_FIRST | AAP_FLAG_LAST):
                raise ValueError(
                    f"Middle frame {idx} has unexpected FIRST/LAST flags: "
                    f"0x{flags:02X}"
                )

        payload_parts.append(payload)

    return first_channel, b"".join(payload_parts)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


class TestAAPFrameBuilding(unittest.TestCase):
    """Build frames and inspect the resulting bytes."""

    def test_single_frame_video(self):
        payload = b"\x00\x00\x00\x01\x67\x42\x00\x1e"  # H.264 SPS NAL
        frame = build_frame(AAP_CHANNEL_VIDEO, AAP_FLAG_SINGLE, payload)

        # Length field should be header + payload
        self.assertEqual(len(frame), AAP_HEADER_SIZE + len(payload))

        # Length bytes (big-endian)
        expected_len = AAP_HEADER_SIZE + len(payload)
        self.assertEqual(struct.unpack_from(">H", frame, 0)[0], expected_len)

        # Channel byte
        self.assertEqual(frame[2], AAP_CHANNEL_VIDEO)

        # Flags byte
        self.assertEqual(frame[3], AAP_FLAG_SINGLE)

        # Payload intact
        self.assertEqual(frame[4:], payload)

    def test_single_frame_control(self):
        payload = b"\x00\x03\x00\x00"  # Synthetic control message
        frame = build_frame(AAP_CHANNEL_CONTROL,
                             AAP_FLAG_SINGLE | AAP_FLAG_CONTROL, payload)

        self.assertEqual(frame[2], AAP_CHANNEL_CONTROL)
        self.assertEqual(frame[3], AAP_FLAG_SINGLE | AAP_FLAG_CONTROL)

    def test_empty_payload(self):
        frame = build_frame(AAP_CHANNEL_INPUT, AAP_FLAG_SINGLE, b"")
        self.assertEqual(len(frame), AAP_HEADER_SIZE)
        total_len = struct.unpack_from(">H", frame, 0)[0]
        self.assertEqual(total_len, AAP_HEADER_SIZE)

    def test_large_payload(self):
        payload = bytes(range(256)) * 10  # 2560 bytes
        frame = build_frame(AAP_CHANNEL_MEDIA, AAP_FLAG_SINGLE, payload)
        self.assertEqual(len(frame), AAP_HEADER_SIZE + len(payload))


class TestAAPFrameParsing(unittest.TestCase):
    """Round-trip: build then parse."""

    def _roundtrip(self, channel, flags, payload):
        frame = build_frame(channel, flags, payload)
        ch_out, fl_out, pl_out = parse_frame(frame)
        self.assertEqual(ch_out, channel)
        self.assertEqual(fl_out, flags)
        self.assertEqual(pl_out, payload)

    def test_roundtrip_video(self):
        self._roundtrip(AAP_CHANNEL_VIDEO, AAP_FLAG_SINGLE,
                        b"\x00\x00\x00\x01\x65" + b"\xAB" * 1024)

    def test_roundtrip_control(self):
        self._roundtrip(AAP_CHANNEL_CONTROL, AAP_FLAG_CONTROL | AAP_FLAG_SINGLE,
                        b"\xDE\xAD\xBE\xEF")

    def test_roundtrip_empty(self):
        self._roundtrip(AAP_CHANNEL_INPUT, AAP_FLAG_SINGLE, b"")

    def test_parse_error_too_short(self):
        with self.assertRaises(ValueError):
            parse_frame(b"\x00")  # < 4 bytes

    def test_parse_error_length_too_small(self):
        # Length field = 2, which is less than minimum 4
        bad = struct.pack(">HBB", 2, 0, 0)
        with self.assertRaises(ValueError):
            parse_frame(bad)

    def test_parse_error_truncated_payload(self):
        # Claim length=20 but only provide 8 bytes
        bad = struct.pack(">HBB", 20, 3, 3) + b"\x00" * 4
        with self.assertRaises(ValueError):
            parse_frame(bad)

    def test_parse_ignores_trailing_bytes(self):
        payload = b"\x01\x02\x03"
        frame = build_frame(AAP_CHANNEL_VIDEO, AAP_FLAG_SINGLE, payload)
        # Append extra bytes — parse should ignore them
        ch, fl, pl = parse_frame(frame + b"\xFF\xFF")
        self.assertEqual(pl, payload)


class TestAAPFragmentSplitting(unittest.TestCase):
    """Test message → multiple frames splitting."""

    def test_single_frame_small_message(self):
        msg = b"Hello"
        frames = split_into_fragments(AAP_CHANNEL_VIDEO, msg,
                                       max_payload=64)
        self.assertEqual(len(frames), 1)
        _, flags, _ = parse_frame(frames[0])
        self.assertEqual(flags, AAP_FLAG_SINGLE)

    def test_single_frame_exact_max(self):
        msg = b"X" * 64
        frames = split_into_fragments(AAP_CHANNEL_VIDEO, msg,
                                       max_payload=64)
        self.assertEqual(len(frames), 1)

    def test_two_fragments(self):
        msg = b"A" * 65  # 1 byte over limit → 2 frames
        frames = split_into_fragments(AAP_CHANNEL_VIDEO, msg, max_payload=64)
        self.assertEqual(len(frames), 2)

        _, flags0, pl0 = parse_frame(frames[0])
        _, flags1, pl1 = parse_frame(frames[1])

        self.assertTrue(flags0 & AAP_FLAG_FIRST)
        self.assertFalse(flags0 & AAP_FLAG_LAST)
        self.assertTrue(flags1 & AAP_FLAG_LAST)
        self.assertFalse(flags1 & AAP_FLAG_FIRST)

        self.assertEqual(pl0 + pl1, msg)

    def test_many_fragments(self):
        msg = bytes(range(256))   # 256 bytes, 4 fragments at 64 each
        frames = split_into_fragments(AAP_CHANNEL_VIDEO, msg, max_payload=64)
        self.assertEqual(len(frames), 4)

        # Verify flags on each frame
        for i, raw in enumerate(frames):
            _, flags, _ = parse_frame(raw)
            if i == 0:
                self.assertTrue(flags & AAP_FLAG_FIRST)
                self.assertFalse(flags & AAP_FLAG_LAST)
            elif i == len(frames) - 1:
                self.assertTrue(flags & AAP_FLAG_LAST)
                self.assertFalse(flags & AAP_FLAG_FIRST)
            else:
                self.assertEqual(flags & (AAP_FLAG_FIRST | AAP_FLAG_LAST), 0)

    def test_empty_message(self):
        frames = split_into_fragments(AAP_CHANNEL_CONTROL, b"")
        self.assertEqual(len(frames), 1)
        _, flags, payload = parse_frame(frames[0])
        self.assertEqual(payload, b"")
        self.assertEqual(flags, AAP_FLAG_SINGLE)


class TestAAPFragmentReassembly(unittest.TestCase):
    """Test fragment reassembly back into a complete message."""

    def _split_and_reassemble(self, message, channel=AAP_CHANNEL_VIDEO,
                               max_payload=64):
        frames = split_into_fragments(channel, message,
                                       max_payload=max_payload)
        ch_out, msg_out = reassemble_fragments(frames)
        self.assertEqual(ch_out, channel)
        self.assertEqual(msg_out, message)
        return frames

    def test_reassemble_single_frame(self):
        self._split_and_reassemble(b"single")

    def test_reassemble_two_frames(self):
        self._split_and_reassemble(b"B" * 65)

    def test_reassemble_many_frames(self):
        large = bytes(range(256)) * 4   # 1024 bytes
        self._split_and_reassemble(large, max_payload=100)

    def test_reassemble_empty(self):
        self._split_and_reassemble(b"")

    def test_reassemble_channel_mismatch_error(self):
        # Build fragments with mismatched channel on second frame
        frames = split_into_fragments(AAP_CHANNEL_VIDEO, b"X" * 65,
                                       max_payload=64)
        # Replace second frame with a different channel
        bad_frame = build_frame(AAP_CHANNEL_CONTROL, AAP_FLAG_LAST,
                                 b"X")
        bad_list = [frames[0], bad_frame]
        with self.assertRaises(ValueError):
            reassemble_fragments(bad_list)

    def test_reassemble_missing_first_flag_error(self):
        # First frame without FIRST flag
        bad = build_frame(AAP_CHANNEL_VIDEO, AAP_FLAG_LAST, b"data")
        with self.assertRaises(ValueError):
            reassemble_fragments([bad])

    def test_reassemble_missing_last_flag_error(self):
        # Build a two-frame sequence but remove LAST flag from second
        frames = split_into_fragments(AAP_CHANNEL_VIDEO, b"Y" * 65,
                                       max_payload=64)
        ch, _, pl = parse_frame(frames[1])
        bad_last = build_frame(ch, 0x00, pl)  # no LAST flag
        with self.assertRaises(ValueError):
            reassemble_fragments([frames[0], bad_last])


class TestAAPFrameVariousSizes(unittest.TestCase):
    """Round-trip with various payload sizes to catch edge cases."""

    def test_sizes(self):
        for size in [0, 1, 3, 4, 255, 256, 1000, 4096, 65535 - AAP_HEADER_SIZE]:
            with self.subTest(size=size):
                payload = bytes(i & 0xFF for i in range(size))
                frame = build_frame(AAP_CHANNEL_VIDEO, AAP_FLAG_SINGLE, payload)
                _, _, recovered = parse_frame(frame)
                self.assertEqual(recovered, payload)


# ---------------------------------------------------------------------------
# Live compositor test (optional — skipped if compositor not running)
# ---------------------------------------------------------------------------

class TestAAPCompositorLive(unittest.TestCase):
    """
    Optional: send framed data to the compositor's video socket and verify
    acceptance.  Skipped automatically if the compositor is not running.
    """

    COMPOSITOR_HOST = "127.0.0.1"
    COMPOSITOR_PORT = 5290

    def setUp(self):
        try:
            s = socket.create_connection(
                (self.COMPOSITOR_HOST, self.COMPOSITOR_PORT), timeout=1
            )
            s.close()
        except OSError:
            self.skipTest("Compositor not reachable at "
                          f"{self.COMPOSITOR_HOST}:{self.COMPOSITOR_PORT}")

    def test_send_status_command(self):
        with socket.create_connection(
            (self.COMPOSITOR_HOST, self.COMPOSITOR_PORT), timeout=3
        ) as s:
            s.sendall(b"STATUS\n")
            data = s.recv(1024)
            self.assertGreater(len(data), 0)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)

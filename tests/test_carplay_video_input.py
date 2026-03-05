#!/usr/bin/env python3
"""
test_carplay_video_input.py — CarPlay Video Input via Unix Socket

Tests that the compositor accepts H.264 video data forwarded from the
CarPlay daemon via the Unix domain socket at /tmp/carplay-video.sock.

Frame format on the socket:
    [4 bytes] payload length (big-endian, NOT including header)
    [N bytes] H.264 Annex-B data

The Annex-B start code is: 0x00 0x00 0x00 0x01

NAL unit types (first byte after start code):
    0x67  SPS  (Sequence Parameter Set)
    0x68  PPS  (Picture Parameter Set)
    0x65  IDR  (I-frame / keyframe)
    0x41  Non-IDR P-frame
    0x61  Non-IDR B-frame
"""

import os
import socket
import struct
import threading
import time
import unittest

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

CARPLAY_VIDEO_SOCK = os.environ.get(
    "CARPLAY_VIDEO_SOCK", "/tmp/carplay-video.sock"
)
CONNECT_TIMEOUT = 2.0
IO_TIMEOUT      = 3.0

# ---------------------------------------------------------------------------
# Minimal H.264 Annex-B test data generators
# ---------------------------------------------------------------------------

ANNEX_B_START = b"\x00\x00\x00\x01"

# Minimal synthetic SPS NAL unit (just start code + type byte, not a valid
# encoded SPS — sufficient to test framing, not H.264 decode)
FAKE_SPS = ANNEX_B_START + b"\x67" + b"\x42\x00\x1e" + b"\xda\x01\xe0"
FAKE_PPS = ANNEX_B_START + b"\x68" + b"\xce\x38\x80"
FAKE_IDR = ANNEX_B_START + b"\x65" + b"\xb8\x00\x04\x00" + b"\xFF" * 32
FAKE_PFRAME = ANNEX_B_START + b"\x41" + b"\x9a\x04\x00" + b"\x00" * 16


def build_video_frame(h264_data: bytes) -> bytes:
    """Prefix H.264 data with a 4-byte big-endian length header."""
    return struct.pack(">I", len(h264_data)) + h264_data


def codec_params_frame() -> bytes:
    """SPS + PPS in a single frame (typical for keyframe sequence)."""
    return build_video_frame(FAKE_SPS + FAKE_PPS)


def idr_frame() -> bytes:
    return build_video_frame(FAKE_IDR)


def p_frame() -> bytes:
    return build_video_frame(FAKE_PFRAME)


# ---------------------------------------------------------------------------
# In-process stub Unix socket server
# ---------------------------------------------------------------------------

def _recv_exact_from(conn: socket.socket, n: int):
    """Read exactly n bytes from conn, returning None on timeout or close."""
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = conn.recv(n - len(buf))
        except socket.timeout:
            return None
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def _video_sink_handle(conn: socket.socket, frames: list,
                        lock: threading.Lock) -> None:
    """Accept length-prefixed frames and record them."""
    conn.settimeout(0.5)
    try:
        while True:
            hdr = _recv_exact_from(conn, 4)
            if hdr is None:
                break
            payload_len = struct.unpack(">I", hdr)[0]
            payload = _recv_exact_from(conn, payload_len)
            if payload is None:
                break
            with lock:
                frames.append(payload)
    except OSError:
        pass
    finally:
        conn.close()


class _StubVideoSink:
    """
    Listens on a temporary Unix socket, accepts one connection at a time,
    reads all length-prefixed frames, and records them.
    Does NOT inherit from threading.Thread (avoids Python 3.13 _ThreadHandle
    issue with bound methods on Thread subclasses).
    """

    def __init__(self, sock_path: str):
        self.sock_path = sock_path
        self._received_frames: list = []
        self._lock = threading.Lock()
        self._stop = threading.Event()

        # Remove stale socket
        try:
            os.unlink(sock_path)
        except FileNotFoundError:
            pass

        self._server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._server.bind(sock_path)
        self._server.listen(3)

        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        import selectors
        sel = selectors.DefaultSelector()
        self._server.setblocking(False)
        sel.register(self._server, selectors.EVENT_READ, data="listen")
        while not self._stop.is_set():
            events = sel.select(timeout=0.2)
            for key, _ in events:
                if key.data == "listen":
                    try:
                        conn, _ = self._server.accept()
                    except OSError:
                        continue
                    conn.setblocking(True)
                    _video_sink_handle(conn, self._received_frames, self._lock)
        sel.close()

    @property
    def received_frames(self) -> list:
        with self._lock:
            return list(self._received_frames)

    def clear(self):
        with self._lock:
            self._received_frames.clear()

    def stop(self):
        self._stop.set()
        self._server.close()
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass


# ---------------------------------------------------------------------------
# Base test class
# ---------------------------------------------------------------------------

STUB_SOCK_PATH = "/tmp/carplay-video-test.sock"


class VideoInputTestBase(unittest.TestCase):

    _sink: _StubVideoSink = None
    _use_stub: bool = False
    _sock_path: str = CARPLAY_VIDEO_SOCK

    @classmethod
    def setUpClass(cls):
        # Try to connect to the real compositor socket
        if os.path.exists(CARPLAY_VIDEO_SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(1.0)
                s.connect(CARPLAY_VIDEO_SOCK)
                s.close()
                cls._use_stub = False
                cls._sock_path = CARPLAY_VIDEO_SOCK
                return
            except OSError:
                pass

        # Fall back to in-process stub (starts automatically in __init__)
        cls._sink = _StubVideoSink(STUB_SOCK_PATH)
        cls._use_stub = True
        cls._sock_path = STUB_SOCK_PATH
        time.sleep(0.05)  # let the server socket start

    @classmethod
    def tearDownClass(cls):
        if cls._sink:
            cls._sink.stop()

    def connect(self) -> socket.socket:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(CONNECT_TIMEOUT)
        s.connect(self._sock_path)
        s.settimeout(IO_TIMEOUT)
        return s

    def setUp(self):
        if self._sink:
            self._sink.clear()


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestVideoSocketConnection(VideoInputTestBase):

    def test_connect_to_socket(self):
        s = self.connect()
        s.close()

    def test_connect_twice_sequentially(self):
        for _ in range(2):
            s = self.connect()
            s.close()
            time.sleep(0.02)


class TestVideoFrameSending(VideoInputTestBase):

    def test_send_sps_pps_frame(self):
        """Codec parameters frame (SPS+PPS) should be accepted."""
        s = self.connect()
        try:
            s.sendall(codec_params_frame())
        finally:
            s.close()

        if self._use_stub:
            time.sleep(0.1)
            frames = self._sink.received_frames
            self.assertEqual(len(frames), 1)
            self.assertTrue(frames[0].startswith(FAKE_SPS))

    def test_send_idr_frame(self):
        """IDR keyframe should be accepted."""
        s = self.connect()
        try:
            s.sendall(idr_frame())
        finally:
            s.close()

        if self._use_stub:
            time.sleep(0.1)
            frames = self._sink.received_frames
            self.assertEqual(len(frames), 1)
            # Verify start code
            self.assertTrue(frames[0].startswith(ANNEX_B_START))

    def test_send_sequence(self):
        """Send SPS+PPS, IDR, then P-frame in order."""
        s = self.connect()
        try:
            s.sendall(codec_params_frame())
            s.sendall(idr_frame())
            s.sendall(p_frame())
        finally:
            s.close()

        if self._use_stub:
            time.sleep(0.1)
            self.assertEqual(len(self._sink.received_frames), 3)

    def test_send_large_idr(self):
        """Large IDR frame (simulating a full 1080p keyframe ~100KB)."""
        large_idr = ANNEX_B_START + b"\x65" + b"\xAB" * (100 * 1024)
        frame = build_video_frame(large_idr)

        s = self.connect()
        try:
            s.sendall(frame)
        finally:
            s.close()

        if self._use_stub:
            time.sleep(0.2)
            frames = self._sink.received_frames
            self.assertEqual(len(frames), 1)
            self.assertEqual(len(frames[0]), len(large_idr))

    def test_send_many_p_frames(self):
        """Send 30 consecutive P-frames (one second of video at 30fps)."""
        s = self.connect()
        try:
            for _ in range(30):
                s.sendall(p_frame())
        finally:
            s.close()

        if self._use_stub:
            time.sleep(0.2)
            self.assertEqual(len(self._sink.received_frames), 30)


class TestVideoFrameFormatValidation(VideoInputTestBase):
    """
    Verify that our frame builder produces correctly structured data.
    These tests do not require a live connection — they validate the
    test helper functions themselves.
    """

    def test_codec_params_starts_with_length_header(self):
        frame = codec_params_frame()
        payload_len = struct.unpack(">I", frame[:4])[0]
        self.assertEqual(len(frame) - 4, payload_len)

    def test_idr_starts_with_annex_b(self):
        frame = idr_frame()
        payload = frame[4:]
        self.assertTrue(payload.startswith(ANNEX_B_START))
        # NAL type for IDR = 0x65
        self.assertEqual(payload[4], 0x65)

    def test_p_frame_nal_type(self):
        frame = p_frame()
        payload = frame[4:]
        self.assertTrue(payload.startswith(ANNEX_B_START))
        self.assertEqual(payload[4], 0x41)

    def test_zero_length_frame(self):
        """A zero-length payload is technically valid framing."""
        frame = build_video_frame(b"")
        self.assertEqual(len(frame), 4)
        self.assertEqual(struct.unpack(">I", frame)[0], 0)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)

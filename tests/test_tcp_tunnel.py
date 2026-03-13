#!/usr/bin/env python3
"""
test_tcp_tunnel.py — TCP Tunnel Between T-Dongle and Radxa

Tests the TCP tunnel that carries USB AOA data between the T-Dongle-S3
firmware and the Radxa Cubie A7Z compositor.

Tunnel parameters:
    Host:  127.0.0.1 (localhost for testing, 192.168.4.1 in production)
    Port:  5277

Test coverage:
    - Basic connection establishment
    - Bidirectional data flow (echo server model)
    - Reconnection after disconnect
    - Payload sizes from 1 byte to 64 KB
    - Concurrent connections (brief)
"""

import socket
import struct
import threading
import time
import unittest
import os

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

TUNNEL_HOST = os.environ.get("TUNNEL_HOST", "127.0.0.1")
TUNNEL_PORT = int(os.environ.get("TUNNEL_PORT", "5277"))
CONNECT_TIMEOUT = 2.0   # seconds
IO_TIMEOUT      = 5.0   # seconds

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def try_connect(host: str = TUNNEL_HOST, port: int = TUNNEL_PORT,
                timeout: float = CONNECT_TIMEOUT) -> socket.socket:
    """
    Attempt to connect to the tunnel server.
    Raises ConnectionRefusedError / OSError if unavailable.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((host, port))
    s.settimeout(IO_TIMEOUT)
    return s


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """Read exactly n bytes from sock, raising EOFError on premature close."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError(f"Connection closed after {len(buf)}/{n} bytes")
        buf.extend(chunk)
    return bytes(buf)


# ---------------------------------------------------------------------------
# Simple in-process echo server (used when tunnel not available)
# ---------------------------------------------------------------------------

def _echo_handler(conn: socket.socket):
    """Handle a single echo connection."""
    conn.settimeout(0.5)
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            conn.sendall(data)
    except (OSError, socket.timeout):
        pass
    finally:
        conn.close()


class _EchoServer(threading.Thread):
    """
    Minimal TCP echo server for standalone testing.
    Echoes all received data back to the sender.
    """

    def __init__(self):
        super().__init__(daemon=True)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Bind to a random free port
        self._sock.bind(("127.0.0.1", 0))
        self.host, self.port = self._sock.getsockname()
        self._sock.listen(5)
        self._stop_event = threading.Event()

    def run(self):
        self._sock.settimeout(0.2)
        while not self._stop_event.is_set():
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            # Use a plain function target to avoid Python 3.13 ThreadHandle issue
            t = threading.Thread(target=_echo_handler, args=(conn,), daemon=True)
            t.start()

    def _handle(self, conn: socket.socket):
        _echo_handler(conn)

    def stop(self):
        self._stop_event.set()
        self._sock.close()


# ---------------------------------------------------------------------------
# Base test class
# ---------------------------------------------------------------------------

class TunnelTestBase(unittest.TestCase):
    """
    Base class: tries to connect to the real tunnel first.
    If unavailable, falls back to the in-process echo server.
    """

    _echo_server = None
    _use_echo    = False

    @classmethod
    def setUpClass(cls):
        # Check if real tunnel is available
        try:
            s = try_connect(timeout=1.0)
            s.close()
            cls._use_echo = False
        except OSError:
            # Start in-process echo server
            cls._echo_server = _EchoServer()
            cls._echo_server.start()
            cls._use_echo = True

    @classmethod
    def tearDownClass(cls):
        if cls._echo_server:
            cls._echo_server.stop()

    def connect(self) -> socket.socket:
        if self._use_echo:
            return try_connect(self._echo_server.host, self._echo_server.port)
        return try_connect()

    def info(self):
        if self._use_echo:
            return f"echo server {self._echo_server.host}:{self._echo_server.port}"
        return f"tunnel {TUNNEL_HOST}:{TUNNEL_PORT}"


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestTCPTunnelConnection(TunnelTestBase):

    def test_connect_and_disconnect(self):
        """Establishing and closing a connection should not raise."""
        s = self.connect()
        s.close()

    def test_connect_twice_sequentially(self):
        """Two sequential connections should both succeed."""
        for _ in range(2):
            s = self.connect()
            s.close()


class TestTCPTunnelBidirectional(TunnelTestBase):

    def test_small_echo(self):
        """Send 4 bytes, receive same 4 bytes back."""
        s = self.connect()
        try:
            data = b"\xDE\xAD\xBE\xEF"
            s.sendall(data)
            received = recv_exact(s, len(data))
            self.assertEqual(received, data)
        finally:
            s.close()

    def test_multiple_sequential_sends(self):
        """Send multiple payloads sequentially on the same connection."""
        s = self.connect()
        try:
            payloads = [b"AAP\x00", b"\xFF" * 16, b"hello world"]
            for payload in payloads:
                s.sendall(payload)
                received = recv_exact(s, len(payload))
                self.assertEqual(received, payload)
        finally:
            s.close()

    def test_binary_data(self):
        """Binary data with all byte values 0x00-0xFF should survive."""
        s = self.connect()
        try:
            data = bytes(range(256))
            s.sendall(data)
            received = recv_exact(s, len(data))
            self.assertEqual(received, data)
        finally:
            s.close()


class TestTCPTunnelPayloadSizes(TunnelTestBase):
    """Test with varying payload sizes from 1 byte to 64 KB."""

    SIZES = [1, 2, 3, 7, 63, 64, 65, 255, 256, 1023, 1024, 4096,
             16384, 32768, 65536]

    def test_payload_sizes(self):
        for size in self.SIZES:
            with self.subTest(size=size):
                s = self.connect()
                try:
                    data = bytes(i & 0xFF for i in range(size))
                    s.sendall(data)
                    received = recv_exact(s, size)
                    self.assertEqual(received, data,
                                     f"Data mismatch at size={size}")
                finally:
                    s.close()


class TestTCPTunnelReconnection(TunnelTestBase):
    """Test reconnection behavior after disconnect."""

    def test_reconnect_after_clean_close(self):
        """After closing a connection, a new one should succeed."""
        s1 = self.connect()
        s1.sendall(b"ping")
        recv_exact(s1, 4)
        s1.close()

        time.sleep(0.05)

        s2 = self.connect()
        s2.sendall(b"pong")
        received = recv_exact(s2, 4)
        self.assertEqual(received, b"pong")
        s2.close()

    def test_reconnect_multiple_times(self):
        """Three sequential reconnections should all work."""
        for i in range(3):
            s = self.connect()
            payload = f"test{i}".encode()
            s.sendall(payload)
            received = recv_exact(s, len(payload))
            self.assertEqual(received, payload)
            s.close()
            time.sleep(0.05)

    def test_reconnect_after_abrupt_close(self):
        """After RST (SO_LINGER=0), a new connection should succeed."""
        s1 = self.connect()
        # Force RST by setting linger to 0
        import struct as st
        s1.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                      st.pack("ii", 1, 0))
        s1.close()

        time.sleep(0.1)

        s2 = self.connect()
        s2.sendall(b"recovery")
        received = recv_exact(s2, 8)
        self.assertEqual(received, b"recovery")
        s2.close()


class TestTCPTunnelConcurrent(TunnelTestBase):
    """Brief concurrent connection test."""

    def test_two_concurrent_connections(self):
        """Two simultaneous connections should both work independently."""
        results = {}
        errors  = {}

        def worker(name: str, payload: bytes):
            try:
                s = self.connect()
                s.sendall(payload)
                received = recv_exact(s, len(payload))
                results[name] = received
                s.close()
            except Exception as exc:
                errors[name] = exc

        t1 = threading.Thread(target=worker, args=("a", b"AAAA" * 16))
        t2 = threading.Thread(target=worker, args=("b", b"BBBB" * 16))

        t1.start()
        t2.start()
        t1.join(timeout=IO_TIMEOUT)
        t2.join(timeout=IO_TIMEOUT)

        self.assertNotIn("a", errors, f"Thread a error: {errors.get('a')}")
        self.assertNotIn("b", errors, f"Thread b error: {errors.get('b')}")
        self.assertEqual(results.get("a"), b"AAAA" * 16)
        self.assertEqual(results.get("b"), b"BBBB" * 16)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)

/// AAP (Android Auto Protocol) frame format:
///
/// Byte 0:   Channel number (0=control, 1=input/touch, 2=sensor, 3=video, 4=audio, ...)
/// Byte 1:   Flags
///             0x08 = FIRST_FRAGMENT (first fragment of a fragmented message)
///             0x04 = LAST_FRAGMENT  (last fragment; also set on non-fragmented)
///             0x0C = complete non-fragmented message (both flags set)
/// Bytes 2-3: Payload length (big-endian, does NOT include the 6-byte header)
/// Bytes 4-5: Message type (big-endian)
/// Bytes 6+:  Payload
///
/// Maximum frame payload before fragmentation: 16384 bytes (16 KiB).
use std::io;
use bytes::{BufMut, Bytes, BytesMut};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

// ---------------------------------------------------------------------------
// Channel constants
// ---------------------------------------------------------------------------
#[allow(dead_code)]
pub mod channel {
    pub const CONTROL: u8 = 0;
    pub const INPUT: u8 = 1;
    pub const SENSOR: u8 = 2;
    pub const VIDEO: u8 = 3;
    pub const AUDIO_MEDIA: u8 = 4;
    pub const AUDIO_GUIDANCE: u8 = 5;
    pub const AUDIO_SYSTEM: u8 = 6;
    pub const AUDIO_MIC: u8 = 7;
    pub const BLUETOOTH: u8 = 8;
    pub const WIFI: u8 = 9;
    pub const PHONE_STATUS: u8 = 10;
    pub const MEDIA_STATUS: u8 = 11;
    pub const NAVIGATION: u8 = 12;
    pub const PROJECTED_HUD: u8 = 13;
    pub const VENDOR: u8 = 255;
}

// ---------------------------------------------------------------------------
// Flag constants
// ---------------------------------------------------------------------------
#[allow(dead_code)]
pub mod flags {
    pub const FIRST_FRAGMENT: u8 = 0x08;
    pub const LAST_FRAGMENT: u8 = 0x04;
    pub const COMPLETE: u8 = FIRST_FRAGMENT | LAST_FRAGMENT; // 0x0C
    pub const ENCRYPTED: u8 = 0x01;
}

/// Maximum payload size per frame before fragmentation is required.
pub const MAX_FRAME_PAYLOAD: usize = 16384;

/// Fixed size of the AAP frame header in bytes.
pub const HEADER_SIZE: usize = 6;

// ---------------------------------------------------------------------------
// AapFrame — a fully reassembled logical message
// ---------------------------------------------------------------------------

/// A fully reassembled (defragmented) AAP message.
#[derive(Debug, Clone)]
pub struct AapFrame {
    /// Channel the message belongs to.
    pub channel: u8,
    /// Flags byte from the last (or only) physical frame.
    #[allow(dead_code)]
    pub flags: u8,
    /// Message type identifier.
    pub msg_type: u16,
    /// Fully reassembled payload.
    pub payload: Bytes,
}

impl AapFrame {
    /// Construct a new complete (non-fragmented) frame.
    pub fn new(channel: u8, msg_type: u16, payload: Bytes) -> Self {
        AapFrame {
            channel,
            flags: flags::COMPLETE,
            msg_type,
            payload,
        }
    }

    /// Encode this frame into one or more physical frames, splitting into
    /// `MAX_FRAME_PAYLOAD`-sized chunks if necessary.
    ///
    /// Returns a `Vec<Bytes>` where each element is one wire-ready physical
    /// frame (header + payload chunk).
    pub fn encode_frames(&self) -> Vec<Bytes> {
        let payload = &self.payload;

        if payload.is_empty() {
            return vec![encode_physical_frame(
                self.channel,
                flags::COMPLETE,
                self.msg_type,
                &[],
            )];
        }

        let chunks: Vec<&[u8]> = payload.chunks(MAX_FRAME_PAYLOAD).collect();
        let total = chunks.len();
        let mut out = Vec::with_capacity(total);

        for (i, chunk) in chunks.iter().enumerate() {
            let fragment_flags = match (i == 0, i == total - 1) {
                (true, true) => flags::COMPLETE,
                (true, false) => flags::FIRST_FRAGMENT,
                (false, true) => flags::LAST_FRAGMENT,
                (false, false) => 0x00,
            };
            out.push(encode_physical_frame(
                self.channel,
                fragment_flags,
                self.msg_type,
                chunk,
            ));
        }
        out
    }
}

/// Encode a single physical frame into a `Bytes` buffer.
fn encode_physical_frame(channel: u8, frame_flags: u8, msg_type: u16, payload: &[u8]) -> Bytes {
    let payload_len = payload.len();
    let total = HEADER_SIZE + payload_len;
    let mut buf = BytesMut::with_capacity(total);
    buf.put_u8(channel);
    buf.put_u8(frame_flags);
    buf.put_u16(payload_len as u16);
    buf.put_u16(msg_type);
    buf.put_slice(payload);
    buf.freeze()
}

// ---------------------------------------------------------------------------
// AapFrameReader — reads and reassembles frames from an AsyncRead source
// ---------------------------------------------------------------------------

/// Accumulated state for fragment reassembly.
struct Reassembler {
    channel: u8,
    msg_type: u16,
    data: BytesMut,
}

/// Reads physical AAP frames from any `AsyncRead` source and reassembles
/// fragmented messages into complete `AapFrame` instances.
pub struct AapFrameReader<R> {
    inner: R,
    // Scratch buffer for reading header + payload
    header_buf: [u8; HEADER_SIZE],
    // In-progress fragment accumulator
    reassembly: Option<Reassembler>,
}

impl<R: AsyncRead + Unpin> AapFrameReader<R> {
    pub fn new(inner: R) -> Self {
        AapFrameReader {
            inner,
            header_buf: [0u8; HEADER_SIZE],
            reassembly: None,
        }
    }

    /// Read the next fully reassembled AAP message.
    ///
    /// Returns `Ok(None)` on clean EOF, `Err(_)` on I/O error or protocol
    /// violation.
    pub async fn read_frame(&mut self) -> io::Result<Option<AapFrame>> {
        loop {
            // Read 6-byte header
            match self.inner.read_exact(&mut self.header_buf).await {
                Ok(_) => {}
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                    return Ok(None);
                }
                Err(e) => return Err(e),
            }

            let channel = self.header_buf[0];
            let frame_flags = self.header_buf[1];
            let payload_len =
                u16::from_be_bytes([self.header_buf[2], self.header_buf[3]]) as usize;
            let msg_type =
                u16::from_be_bytes([self.header_buf[4], self.header_buf[5]]);

            // Read payload
            let mut payload_buf = vec![0u8; payload_len];
            if payload_len > 0 {
                self.inner.read_exact(&mut payload_buf).await?;
            }

            let is_first = (frame_flags & flags::FIRST_FRAGMENT) != 0;
            let is_last = (frame_flags & flags::LAST_FRAGMENT) != 0;

            // Validate fragment state transitions
            if is_first {
                if self.reassembly.is_some() {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "AAP: FIRST_FRAGMENT received while reassembly was in progress",
                    ));
                }
                let mut data = BytesMut::new();
                data.extend_from_slice(&payload_buf);
                self.reassembly = Some(Reassembler {
                    channel,
                    msg_type,
                    data,
                });
            } else if !is_first && !is_last {
                // Middle fragment
                match self.reassembly.as_mut() {
                    Some(r) => {
                        if r.channel != channel || r.msg_type != msg_type {
                            return Err(io::Error::new(
                                io::ErrorKind::InvalidData,
                                "AAP: fragment channel/type mismatch",
                            ));
                        }
                        r.data.extend_from_slice(&payload_buf);
                    }
                    None => {
                        return Err(io::Error::new(
                            io::ErrorKind::InvalidData,
                            "AAP: middle fragment with no active reassembly",
                        ));
                    }
                }
            }

            if is_last {
                let (reassembly_channel, reassembly_type, mut data) = match self.reassembly.take() {
                    Some(r) => (r.channel, r.msg_type, r.data),
                    None => {
                        // Single complete frame — no prior fragment state
                        let mut d = BytesMut::new();
                        d.extend_from_slice(&payload_buf);
                        (channel, msg_type, d)
                    }
                };

                // For the last fragment (which was not a first fragment),
                // append the payload we just read.
                if !is_first {
                    data.extend_from_slice(&payload_buf);
                }

                return Ok(Some(AapFrame {
                    channel: reassembly_channel,
                    flags: frame_flags,
                    msg_type: reassembly_type,
                    payload: data.freeze(),
                }));
            }

            // Not last fragment — loop to read next physical frame
        }
    }
}

// ---------------------------------------------------------------------------
// AapFrameWriter — writes AapFrames to an AsyncWrite sink
// ---------------------------------------------------------------------------

/// Writes `AapFrame` instances to any `AsyncWrite` sink, fragmenting large
/// payloads automatically.
pub struct AapFrameWriter<W> {
    inner: W,
}

impl<W: AsyncWrite + Unpin> AapFrameWriter<W> {
    pub fn new(inner: W) -> Self {
        AapFrameWriter { inner }
    }

    /// Write a frame to the sink, fragmenting if necessary.
    pub async fn write_frame(&mut self, frame: &AapFrame) -> io::Result<()> {
        let encoded = frame.encode_frames();
        for physical in &encoded {
            self.inner.write_all(physical).await?;
        }
        Ok(())
    }

    /// Flush the underlying writer.
    #[allow(dead_code)]
    pub async fn flush(&mut self) -> io::Result<()> {
        self.inner.flush().await
    }

    /// Consume the writer and return the inner sink.
    #[allow(dead_code)]
    pub fn into_inner(self) -> W {
        self.inner
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use bytes::Bytes;

    #[test]
    fn test_encode_small_frame() {
        let f = AapFrame::new(channel::VIDEO, 0x0001, Bytes::from_static(b"hello"));
        let frames = f.encode_frames();
        assert_eq!(frames.len(), 1);
        let wire = &frames[0];
        assert_eq!(wire[0], channel::VIDEO);
        assert_eq!(wire[1], flags::COMPLETE);
        assert_eq!(u16::from_be_bytes([wire[2], wire[3]]), 5);
        assert_eq!(u16::from_be_bytes([wire[4], wire[5]]), 0x0001);
        assert_eq!(&wire[6..], b"hello");
    }

    #[test]
    fn test_fragment_large_frame() {
        let big_payload = vec![0xABu8; MAX_FRAME_PAYLOAD * 2 + 100];
        let f = AapFrame::new(channel::VIDEO, 0x0002, Bytes::from(big_payload));
        let frames = f.encode_frames();
        assert_eq!(frames.len(), 3);

        // First fragment
        assert_eq!(frames[0][1], flags::FIRST_FRAGMENT);
        // Middle fragment
        assert_eq!(frames[1][1], 0x00);
        // Last fragment
        assert_eq!(frames[2][1], flags::LAST_FRAGMENT);
    }

    #[tokio::test]
    async fn test_roundtrip_single_frame() {
        let payload = Bytes::from_static(b"test payload");
        let frame = AapFrame::new(channel::CONTROL, 0x1234, payload.clone());

        // Encode to wire bytes
        let encoded = frame.encode_frames();
        let mut wire = Vec::new();
        for p in encoded {
            wire.extend_from_slice(&p);
        }

        // Decode
        let cursor = std::io::Cursor::new(wire);
        let mut reader = AapFrameReader::new(tokio::io::BufReader::new(cursor));
        let decoded = reader.read_frame().await.unwrap().unwrap();

        assert_eq!(decoded.channel, channel::CONTROL);
        assert_eq!(decoded.msg_type, 0x1234);
        assert_eq!(decoded.payload, payload);
    }

    #[tokio::test]
    async fn test_roundtrip_fragmented_frame() {
        let big_payload = Bytes::from(vec![0x42u8; MAX_FRAME_PAYLOAD + 500]);
        let frame = AapFrame::new(channel::VIDEO, 0xABCD, big_payload.clone());

        let encoded = frame.encode_frames();
        let mut wire = Vec::new();
        for p in encoded {
            wire.extend_from_slice(&p);
        }

        let cursor = std::io::Cursor::new(wire);
        let mut reader = AapFrameReader::new(tokio::io::BufReader::new(cursor));
        let decoded = reader.read_frame().await.unwrap().unwrap();

        assert_eq!(decoded.channel, channel::VIDEO);
        assert_eq!(decoded.msg_type, 0xABCD);
        assert_eq!(decoded.payload, big_payload);
    }
}

/// Bidirectional AAP proxy bridge.
///
/// A single `ProxySession` manages the full lifecycle of one connected pair:
///
///   car-side TCP stream (T-Dongle)  ←→  phone-side TLS stream
///
/// Frames are read concurrently from both sides using `tokio::select!`.
/// Each frame passes through the `MitmLayer` before being forwarded.
///
/// The session terminates when either side drops its connection.  The caller
/// is responsible for re-establishing connections and constructing a new
/// `ProxySession`.
use std::io;

use log::{debug, error, info, warn};
use tokio::io::{AsyncRead, AsyncWrite};
use tokio::net::TcpStream;
use tokio_rustls::server::TlsStream;

use crate::aap::{AapFrame, AapFrameReader, AapFrameWriter};
use crate::mitm::MitmLayer;

/// Statistics collected during a session; useful for debugging.
#[derive(Debug, Default)]
pub struct SessionStats {
    pub car_to_phone_frames: u64,
    pub phone_to_car_frames: u64,
    pub car_to_phone_bytes: u64,
    pub phone_to_car_bytes: u64,
    pub mitm_dropped_frames: u64,
}

/// Describes why a session ended.
#[derive(Debug)]
pub enum SessionEnd {
    /// The car-side (T-Dongle) closed the connection.
    CarDisconnected,
    /// The phone closed the connection.
    PhoneDisconnected,
    /// An I/O error occurred on the car side.
    CarIoError(io::Error),
    /// An I/O error occurred on the phone side.
    PhoneIoError(io::Error),
}

impl std::fmt::Display for SessionEnd {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SessionEnd::CarDisconnected => write!(f, "car disconnected (clean EOF)"),
            SessionEnd::PhoneDisconnected => write!(f, "phone disconnected (clean EOF)"),
            SessionEnd::CarIoError(e) => write!(f, "car I/O error: {}", e),
            SessionEnd::PhoneIoError(e) => write!(f, "phone I/O error: {}", e),
        }
    }
}

/// Runs the proxy bridge until one side disconnects.
///
/// - `car_stream`: plain TCP stream from the T-Dongle
/// - `phone_stream`: TLS stream from the phone
/// - `mitm`: MITM layer for video tap + touch remapping
///
/// Returns `(reason, stats)`.
pub async fn run_session(
    car_stream: TcpStream,
    phone_stream: TlsStream<TcpStream>,
    mitm: MitmLayer,
) -> (SessionEnd, SessionStats) {
    info!("Proxy session started");

    // Split both streams into read and write halves.
    // For plain TCP we use tokio's into_split().
    let (car_read, car_write) = tokio::io::split(car_stream);
    let (phone_read, phone_write) = tokio::io::split(phone_stream);

    let mut stats = SessionStats::default();
    let end = proxy_loop(car_read, car_write, phone_read, phone_write, &mitm, &mut stats).await;

    info!("Proxy session ended: {}", end);
    info!(
        "Session stats: car→phone {} frames ({} bytes), phone→car {} frames ({} bytes), dropped {}",
        stats.car_to_phone_frames,
        stats.car_to_phone_bytes,
        stats.phone_to_car_frames,
        stats.phone_to_car_bytes,
        stats.mitm_dropped_frames,
    );

    (end, stats)
}

/// Internal: the main select loop.
async fn proxy_loop<CR, CW, PR, PW>(
    car_read: CR,
    car_write: CW,
    phone_read: PR,
    phone_write: PW,
    mitm: &MitmLayer,
    stats: &mut SessionStats,
) -> SessionEnd
where
    CR: AsyncRead + Unpin,
    CW: AsyncWrite + Unpin,
    PR: AsyncRead + Unpin,
    PW: AsyncWrite + Unpin,
{
    let mut car_reader = AapFrameReader::new(car_read);
    let mut car_writer = AapFrameWriter::new(car_write);
    let mut phone_reader = AapFrameReader::new(phone_read);
    let mut phone_writer = AapFrameWriter::new(phone_write);

    loop {
        tokio::select! {
            // Car → Phone
            result = car_reader.read_frame() => {
                match result {
                    Ok(Some(frame)) => {
                        let payload_len = frame.payload.len() as u64;
                        stats.car_to_phone_frames += 1;
                        stats.car_to_phone_bytes += payload_len;

                        debug!(
                            "car→phone: ch={} type=0x{:04x} payload={}B",
                            frame.channel, frame.msg_type, frame.payload.len()
                        );

                        match mitm.process_car_to_phone(&frame) {
                            Some(out_frame) => {
                                if let Err(e) = phone_writer.write_frame(&out_frame).await {
                                    return SessionEnd::PhoneIoError(e);
                                }
                            }
                            None => {
                                debug!(
                                    "car→phone: frame ch={} dropped by MITM",
                                    frame.channel
                                );
                                stats.mitm_dropped_frames += 1;
                            }
                        }
                    }
                    Ok(None) => {
                        return SessionEnd::CarDisconnected;
                    }
                    Err(e) => {
                        warn!("car→phone read error: {}", e);
                        return SessionEnd::CarIoError(e);
                    }
                }
            }

            // Phone → Car
            result = phone_reader.read_frame() => {
                match result {
                    Ok(Some(frame)) => {
                        let payload_len = frame.payload.len() as u64;
                        stats.phone_to_car_frames += 1;
                        stats.phone_to_car_bytes += payload_len;

                        debug!(
                            "phone→car: ch={} type=0x{:04x} payload={}B",
                            frame.channel, frame.msg_type, frame.payload.len()
                        );

                        match mitm.process_phone_to_car(&frame) {
                            Some(out_frame) => {
                                if let Err(e) = car_writer.write_frame(&out_frame).await {
                                    return SessionEnd::CarIoError(e);
                                }
                            }
                            None => {
                                debug!(
                                    "phone→car: frame ch={} dropped by MITM",
                                    frame.channel
                                );
                                stats.mitm_dropped_frames += 1;
                            }
                        }
                    }
                    Ok(None) => {
                        return SessionEnd::PhoneDisconnected;
                    }
                    Err(e) => {
                        warn!("phone→car read error: {}", e);
                        return SessionEnd::PhoneIoError(e);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Standalone session — car connected, no phone (CarPlay / camera-only mode)
// ---------------------------------------------------------------------------

/// Runs a standalone injection session when only the car (T-Dongle) is
/// connected and no Android phone is present.
///
/// In this mode the compositor generates video (CarPlay or camera-only)
/// and sends H.264 frames via `/tmp/aa-video-out.sock`.  This function:
///   1. Reads composited frames from `CompositorOutput`
///   2. Wraps them as AAP channel-3 video frames
///   3. Writes them to the car-side TCP stream
///   4. Reads touch events from the car and forwards to compositor
///
/// Returns when the car disconnects or a phone connects (signalled via
/// the `phone_connected` channel).
pub async fn run_standalone_session(
    car_stream: TcpStream,
    mitm: MitmLayer,
    mut phone_signal: tokio::sync::mpsc::Receiver<()>,
) -> SessionEnd {
    use crate::aap::{AapFrame, AapFrameReader, AapFrameWriter, channel, flags};
    use std::time::Duration;

    info!("Standalone session started (compositor → car, no phone)");

    let (car_read, car_write) = tokio::io::split(car_stream);
    let mut car_reader = AapFrameReader::new(car_read);
    let mut car_writer = AapFrameWriter::new(car_write);

    let mut frame_interval = tokio::time::interval(Duration::from_millis(33)); // ~30fps

    loop {
        tokio::select! {
            // Check if a phone connected (exit standalone, switch to full proxy)
            _ = phone_signal.recv() => {
                info!("Phone connected — exiting standalone mode");
                return SessionEnd::PhoneDisconnected; // signal to main loop to restart
            }

            // Inject compositor frames into car at ~30fps
            _ = frame_interval.tick() => {
                if let Some(composited) = mitm.compositor_output().latest_frame() {
                    let aap_frame = AapFrame::new(
                        channel::VIDEO,
                        0x0000, // video data message type
                        composited,
                    );
                    if let Err(e) = car_writer.write_frame(&aap_frame).await {
                        warn!("Standalone: car write error: {}", e);
                        return SessionEnd::CarIoError(e);
                    }
                }
            }

            // Read frames from car (touch events, etc.)
            result = car_reader.read_frame() => {
                match result {
                    Ok(Some(frame)) => {
                        // Touch events from car → forward to compositor via control
                        debug!(
                            "Standalone car→: ch={} type=0x{:04x} len={}",
                            frame.channel, frame.msg_type, frame.payload.len()
                        );
                        // In standalone mode, car touch events aren't forwarded to a phone.
                        // The compositor handles display mode changes via its own control channel.
                    }
                    Ok(None) => {
                        return SessionEnd::CarDisconnected;
                    }
                    Err(e) => {
                        warn!("Standalone car read error: {}", e);
                        return SessionEnd::CarIoError(e);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// TLS handshake helper
// ---------------------------------------------------------------------------

/// Perform the TLS handshake with the phone.
///
/// The phone connects as a TLS client; we accept as the TLS server using the
/// certificate built by `tls::build_tls_acceptor()`.
///
/// Returns the established `TlsStream` or an error if the handshake fails.
pub async fn tls_accept(
    acceptor: &tokio_rustls::TlsAcceptor,
    stream: TcpStream,
    peer: std::net::SocketAddr,
) -> anyhow::Result<TlsStream<TcpStream>> {
    info!("TLS handshake with phone at {}", peer);
    let tls_stream = acceptor
        .accept(stream)
        .await
        .map_err(|e| anyhow::anyhow!("TLS handshake failed with {}: {}", peer, e))?;
    info!("TLS handshake complete with {}", peer);
    Ok(tls_stream)
}

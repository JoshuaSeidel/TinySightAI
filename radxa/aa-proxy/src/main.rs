/// aa-proxy — Android Auto Protocol proxy for the ESP wireless car dongle.
///
/// ## Architecture
///
/// ```
/// T-Dongle (car USB)  ──TCP:5277──►  aa-proxy  ◄──TCP:5288──  Phone (AA over WiFi)
///                                       │
///                                       ▼ Unix socket
///                                  Compositor (video)  /tmp/aa-video.sock
///                                  Compositor (ctrl)   /tmp/aa-control.sock
/// ```
///
/// ## Reconnection
///
/// When either side drops the connection the proxy loops back and waits for
/// the car to reconnect.  Two operating modes:
///
/// - **Full proxy** — car + phone both connected.  Frames are relayed
///   bidirectionally with MITM video compositing and touch remapping.
/// - **Standalone** — car connected, no phone within 5 seconds.  The
///   compositor generates video (CarPlay / camera-only) and aa-proxy injects
///   those frames into the car-bound AAP video channel.  When a phone later
///   connects, standalone exits and both sides reconnect for full proxy mode.
///
/// ## Environment variables
///
/// | Variable            | Default               | Description                          |
/// |---------------------|-----------------------|--------------------------------------|
/// | `CAR_PORT`          | `5277`                | TCP port for T-Dongle (car side)     |
/// | `PHONE_PORT`        | `5288`                | TCP port for phone side              |
/// | `VIDEO_SOCK`        | `/tmp/aa-video.sock`     | Unix socket path for video tap          |
/// | `VIDEO_OUT_SOCK`    | `/tmp/aa-video-out.sock` | Unix socket path for composited output  |
/// | `CONTROL_SOCK`      | `/tmp/aa-control.sock`   | Unix socket path for control msgs       |
/// | `RUST_LOG`          | `info`                   | Log level (error/warn/info/debug)       |
use std::net::SocketAddr;
use std::time::Duration;

use anyhow::{Context, Result};
use log::{error, info, warn};
use tokio::net::{TcpListener, TcpStream};

mod aap;
mod mitm;
mod proxy;
mod tls;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    car_port: u16,
    phone_port: u16,
    video_sock: String,
    /// Unix socket path where the compositor sends composited H.264 frames
    /// back to aa-proxy for injection into the car-bound AAP video channel.
    compositor_out_sock: String,
    control_sock: String,
}

impl Config {
    fn from_env() -> Self {
        Config {
            car_port: env_u16("CAR_PORT", 5277),
            phone_port: env_u16("PHONE_PORT", 5288),
            video_sock: std::env::var("VIDEO_SOCK")
                .unwrap_or_else(|_| "/tmp/aa-video.sock".to_string()),
            compositor_out_sock: std::env::var("VIDEO_OUT_SOCK")
                .unwrap_or_else(|_| "/tmp/aa-video-out.sock".to_string()),
            control_sock: std::env::var("CONTROL_SOCK")
                .unwrap_or_else(|_| "/tmp/aa-control.sock".to_string()),
        }
    }
}

fn env_u16(name: &str, default: u16) -> u16 {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() -> Result<()> {
    // Initialise logging; default to info level.
    if std::env::var("RUST_LOG").is_err() {
        std::env::set_var("RUST_LOG", "info");
    }
    env_logger::init();

    info!("aa-proxy starting (version {})", env!("CARGO_PKG_VERSION"));

    let config = Config::from_env();

    // Build TLS acceptor once at startup — persisted cert is reused across
    // sessions.
    let tls_acceptor = tls::build_tls_acceptor()
        .context("Failed to build TLS acceptor")?;

    // Shared display mode — updated by control socket, read by MITM layer.
    let shared_mode = mitm::new_shared_mode();

    // Spawn control socket listener (background task).
    mitm::spawn_control_listener(&config.control_sock, shared_mode.clone());

    // Bind listeners for car-side and phone-side connections.
    let car_addr: SocketAddr = format!("0.0.0.0:{}", config.car_port).parse()?;
    let phone_addr: SocketAddr = format!("0.0.0.0:{}", config.phone_port).parse()?;

    let car_listener = TcpListener::bind(car_addr)
        .await
        .with_context(|| format!("Failed to bind car-side listener on {}", car_addr))?;
    let phone_listener = TcpListener::bind(phone_addr)
        .await
        .with_context(|| format!("Failed to bind phone-side listener on {}", phone_addr))?;

    info!("Listening for T-Dongle (car) on {}", car_addr);
    info!("Listening for phone on {}", phone_addr);

    // Main reconnection loop.
    //
    // 1. Wait for the car (T-Dongle) to connect.
    // 2. Try to accept a phone within 5 seconds.
    // 3. Phone connected → full proxy mode (AA relay).
    //    Timeout → standalone mode (compositor video → car).
    //    If a phone later connects during standalone, both sides
    //    reconnect and switch to full proxy on the next iteration.
    loop {
        // Step 1: Wait for the car (T-Dongle).
        let (car_stream, car_peer) = match accept_one(&car_listener).await {
            Ok(pair) => pair,
            Err(e) => {
                error!("Car accept error: {}; retrying in 2s", e);
                tokio::time::sleep(Duration::from_secs(2)).await;
                continue;
            }
        };
        info!("Car connected from {}", car_peer);

        // Step 2: Wait for phone with 5-second timeout.
        match tokio::time::timeout(Duration::from_secs(5), accept_one(&phone_listener)).await {
            // Phone connected in time → full proxy mode.
            Ok(Ok((phone_raw, phone_peer))) => {
                info!("Phone connected from {}", phone_peer);
                match proxy::tls_accept(&tls_acceptor, phone_raw, phone_peer).await {
                    Ok(phone_tls) => {
                        let mitm_layer = mitm::MitmLayer::new(
                            &config.video_sock,
                            &config.compositor_out_sock,
                            shared_mode.clone(),
                        );
                        let (end, _stats) =
                            proxy::run_session(car_stream, phone_tls, mitm_layer).await;
                        info!("Session ended: {}", end);
                    }
                    Err(e) => {
                        error!("TLS handshake failed: {}; reconnecting", e);
                    }
                }
            }
            Ok(Err(e)) => {
                error!("Phone accept error: {}; reconnecting", e);
            }
            // Timeout → standalone mode (compositor → car, no phone).
            Err(_) => {
                info!("No phone within 5s — entering standalone mode");
                let (_phone_tx, phone_rx) = tokio::sync::mpsc::channel::<()>(1);
                let mitm_layer = mitm::MitmLayer::new(
                    &config.video_sock,
                    &config.compositor_out_sock,
                    shared_mode.clone(),
                );

                // Run standalone session and phone-accept concurrently.
                // If a phone connects, standalone is cancelled; both sides
                // reconnect on the next loop iteration.
                tokio::select! {
                    end = proxy::run_standalone_session(car_stream, mitm_layer, phone_rx) => {
                        info!("Standalone session ended: {}", end);
                    }
                    result = accept_one(&phone_listener) => {
                        match result {
                            Ok((_, peer)) => {
                                info!("Phone {} connected — leaving standalone", peer);
                                // Standalone is cancelled by select!, car stream dropped.
                                // Both sides will reconnect on next loop iteration.
                            }
                            Err(e) => error!("Phone accept error in standalone: {}", e),
                        }
                    }
                }
            }
        }

        info!("Waiting for next connection...");
    }
}

/// Accept a single TCP connection and configure the socket.
async fn accept_one(
    listener: &TcpListener,
) -> Result<(TcpStream, SocketAddr)> {
    let (stream, addr) = listener.accept().await.context("TCP accept error")?;

    // TCP_NODELAY reduces latency for small AAP control frames.
    if let Err(e) = stream.set_nodelay(true) {
        warn!("Failed to set TCP_NODELAY for {}: {}", addr, e);
    }

    Ok((stream, addr))
}

// ---------------------------------------------------------------------------
// Signal handling (graceful shutdown on SIGTERM / SIGINT)
// ---------------------------------------------------------------------------

#[allow(dead_code)]
async fn wait_for_shutdown() {
    #[cfg(unix)]
    {
        use tokio::signal::unix::{signal, SignalKind};
        let mut sigterm = signal(SignalKind::terminate())
            .expect("Failed to register SIGTERM handler");
        let mut sigint = signal(SignalKind::interrupt())
            .expect("Failed to register SIGINT handler");
        tokio::select! {
            _ = sigterm.recv() => { info!("Received SIGTERM; shutting down"); }
            _ = sigint.recv()  => { info!("Received SIGINT; shutting down"); }
        }
    }
    #[cfg(not(unix))]
    {
        tokio::signal::ctrl_c()
            .await
            .expect("Failed to install Ctrl-C handler");
        info!("Received Ctrl-C; shutting down");
    }
}

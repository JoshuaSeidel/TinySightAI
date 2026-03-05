/// MITM tap for video compositing and touch-coordinate remapping.
///
/// # Video tap  (phone → car, channel 3)
/// Intercepted video H.264 frames are forwarded to the compositor via a Unix
/// domain socket at `/tmp/aa-video.sock`.  If the compositor is not
/// connected the frames pass through to the car unchanged — there is no
/// back-pressure or blocking on the compositor path.
///
/// # Touch remapping  (car → phone, channel 1)
/// When split-screen is active the car's touch coordinates (which cover the
/// full screen) must be remapped so that only the AA region of the screen is
/// forwarded to the phone.  The active layout is received from the compositor
/// via a control socket at `/tmp/aa-control.sock`.
///
/// # Control socket protocol  (`/tmp/aa-control.sock`)
/// Newline-delimited JSON messages from the compositor:
///
/// ```json
/// {"mode":"full_aa"}
/// {"mode":"split_aa_cam","aa_region":{"x":0,"y":0,"w":1280,"h":480},"screen":{"w":1280,"h":800}}
/// {"mode":"split_carplay_cam","aa_region":{"x":0,"y":0,"w":1280,"h":480},"screen":{"w":1280,"h":800}}
/// {"mode":"full_cam"}
/// ```
///
/// The proxy only cares about `aa_region` and `screen` dimensions for touch
/// remapping; it does not need to understand CarPlay layout details.
use std::sync::{Arc, Mutex, RwLock};

use bytes::{Bytes, BytesMut};
use log::{debug, warn};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{UnixListener, UnixStream};
use tokio::sync::mpsc;

use crate::aap::channel;

// ---------------------------------------------------------------------------
// Layout / display mode
// ---------------------------------------------------------------------------

/// A simple rectangle in screen-pixel coordinates.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

/// Screen dimensions (width × height in pixels).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ScreenSize {
    pub w: u32,
    pub h: u32,
}

/// The active display mode as understood by the proxy.
#[derive(Debug, Clone, PartialEq)]
pub enum DisplayMode {
    /// Full-screen AA — touch passes through unchanged.
    FullAa,
    /// Split screen: AA is shown in `aa_region` of a `screen`-sized display.
    SplitAaCam {
        aa_region: Rect,
        screen: ScreenSize,
    },
    /// Same split geometry used for CarPlay side; AA region is identical.
    SplitCarplayCam {
        aa_region: Rect,
        screen: ScreenSize,
    },
    /// Baby cam only — AA not visible; swallow all touch events.
    FullCam,
}

impl DisplayMode {
    /// Return the AA region (if any) within which touches are valid.
    pub fn aa_region(&self) -> Option<(Rect, ScreenSize)> {
        match self {
            DisplayMode::FullAa => None,
            DisplayMode::SplitAaCam { aa_region, screen }
            | DisplayMode::SplitCarplayCam { aa_region, screen } => {
                Some((*aa_region, *screen))
            }
            DisplayMode::FullCam => None,
        }
    }

    /// Returns `true` if AA is visible and touch events should be forwarded.
    pub fn aa_visible(&self) -> bool {
        !matches!(self, DisplayMode::FullCam)
    }
}

impl Default for DisplayMode {
    fn default() -> Self {
        DisplayMode::FullAa
    }
}

// ---------------------------------------------------------------------------
// Shared state accessed by the proxy bridge and the control-socket listener
// ---------------------------------------------------------------------------

/// Shared, cheaply-cloneable reference to the current display mode.
pub type SharedMode = Arc<RwLock<DisplayMode>>;

pub fn new_shared_mode() -> SharedMode {
    Arc::new(RwLock::new(DisplayMode::default()))
}

// ---------------------------------------------------------------------------
// Touch coordinate remapping
// ---------------------------------------------------------------------------

/// AAP touch event message type on channel 1.
const MSG_TYPE_TOUCH: u16 = 0x0001;

/// Remap an absolute touch point `(tx, ty)` from the full-screen coordinate
/// space into the AA-region coordinate space.
///
/// Returns `None` if the touch falls outside the AA region (should be
/// discarded) or if no remapping is active.
pub fn remap_touch(
    tx: u32,
    ty: u32,
    mode: &DisplayMode,
) -> Option<(u32, u32)> {
    match mode.aa_region() {
        None => {
            // Full-screen AA — pass through unchanged.
            Some((tx, ty))
        }
        Some((region, _screen)) => {
            // Touch must land inside the AA region.
            if tx < region.x
                || ty < region.y
                || tx >= region.x + region.w
                || ty >= region.y + region.h
            {
                debug!(
                    "Touch ({},{}) outside AA region {:?} — discarding",
                    tx, ty, region
                );
                return None;
            }
            // Translate to region-local coordinates.
            let local_x = tx - region.x;
            let local_y = ty - region.y;
            Some((local_x, local_y))
        }
    }
}

/// Parse the x/y coordinates out of an AAP touch payload (best-effort).
///
/// AAP touch payloads are protobuf-encoded.  This function performs a
/// minimal hand-rolled parse sufficient for coordinate extraction without
/// pulling in a full protobuf library.
///
/// Touch message layout (simplified, varint fields):
///   field 1 (action_type): varint
///   field 2 (pointer_count): varint
///   field 3 (pointer data, embedded message):
///       field 1 (pointer_id): varint
///       field 2 (x): varint  ← what we want
///       field 3 (y): varint  ← what we want
///       field 4 (pressure): varint
///
/// Returns `(x, y)` of the first pointer, or `None` if parsing fails.
pub fn parse_touch_xy(payload: &[u8]) -> Option<(u32, u32)> {
    // We do a best-effort scan for field 3 (wire type 2 = length-delimited)
    // then parse the embedded message for fields 2 and 3.
    parse_protobuf_touch(payload)
}

fn read_varint(data: &[u8], pos: &mut usize) -> Option<u64> {
    let mut result: u64 = 0;
    let mut shift = 0u32;
    loop {
        if *pos >= data.len() {
            return None;
        }
        let byte = data[*pos];
        *pos += 1;
        result |= ((byte & 0x7F) as u64) << shift;
        if byte & 0x80 == 0 {
            return Some(result);
        }
        shift += 7;
        if shift >= 64 {
            return None;
        }
    }
}

fn parse_protobuf_touch(data: &[u8]) -> Option<(u32, u32)> {
    let mut pos = 0usize;
    while pos < data.len() {
        let tag = read_varint(data, &mut pos)?;
        let field_number = (tag >> 3) as u32;
        let wire_type = (tag & 0x07) as u32;
        match wire_type {
            0 => {
                // varint — skip
                read_varint(data, &mut pos)?;
            }
            1 => {
                // 64-bit — skip
                if pos + 8 > data.len() {
                    return None;
                }
                pos += 8;
            }
            2 => {
                // length-delimited
                let len = read_varint(data, &mut pos)? as usize;
                if pos + len > data.len() {
                    return None;
                }
                if field_number == 3 {
                    // This is the pointer sub-message
                    let sub = &data[pos..pos + len];
                    let mut spos = 0usize;
                    let mut x: Option<u32> = None;
                    let mut y: Option<u32> = None;
                    while spos < sub.len() {
                        let stag = read_varint(sub, &mut spos)?;
                        let sfn = (stag >> 3) as u32;
                        let swt = (stag & 0x07) as u32;
                        match swt {
                            0 => {
                                let val = read_varint(sub, &mut spos)?;
                                if sfn == 2 {
                                    x = Some(val as u32);
                                } else if sfn == 3 {
                                    y = Some(val as u32);
                                }
                            }
                            1 => {
                                if spos + 8 > sub.len() {
                                    return None;
                                }
                                spos += 8;
                            }
                            2 => {
                                let slen = read_varint(sub, &mut spos)? as usize;
                                if spos + slen > sub.len() {
                                    return None;
                                }
                                spos += slen;
                            }
                            5 => {
                                if spos + 4 > sub.len() {
                                    return None;
                                }
                                spos += 4;
                            }
                            _ => return None,
                        }
                        if x.is_some() && y.is_some() {
                            return Some((x.unwrap(), y.unwrap()));
                        }
                    }
                    return match (x, y) {
                        (Some(xv), Some(yv)) => Some((xv, yv)),
                        _ => None,
                    };
                }
                pos += len;
            }
            5 => {
                // 32-bit — skip
                if pos + 4 > data.len() {
                    return None;
                }
                pos += 4;
            }
            _ => return None,
        }
    }
    None
}

/// Encode a remapped `(x, y)` back into the original touch payload by
/// rewriting varint values in place.
///
/// This is intentionally conservative: if we cannot find and patch the
/// coordinates cleanly we return the original payload unchanged rather than
/// corrupting the stream.
pub fn patch_touch_xy(payload: &[u8], new_x: u32, new_y: u32) -> Bytes {
    // Attempt the patch; fall back to original on any error.
    if let Some(patched) = try_patch_touch_xy(payload, new_x, new_y) {
        Bytes::from(patched)
    } else {
        warn!("touch coordinate patching failed; forwarding original payload");
        Bytes::copy_from_slice(payload)
    }
}

fn try_patch_touch_xy(data: &[u8], new_x: u32, new_y: u32) -> Option<Vec<u8>> {
    // We need to rebuild the protobuf because varint sizes may change.
    // Strategy: copy all fields verbatim except field 3 (pointer sub-message),
    // which we rebuild with the new x/y.
    let mut out: Vec<u8> = Vec::with_capacity(data.len());
    let mut pos = 0usize;

    while pos < data.len() {
        let tag_start = pos;
        let tag = read_varint(data, &mut pos)?;
        let field_number = (tag >> 3) as u32;
        let wire_type = (tag & 0x07) as u32;

        match wire_type {
            0 => {
                let val_start = pos;
                read_varint(data, &mut pos)?;
                // Copy tag + varint verbatim
                out.extend_from_slice(&data[tag_start..pos]);
                let _ = val_start;
            }
            1 => {
                if pos + 8 > data.len() {
                    return None;
                }
                out.extend_from_slice(&data[tag_start..pos + 8]);
                pos += 8;
            }
            2 => {
                let len_varint_start = pos;
                let len = read_varint(data, &mut pos)? as usize;
                if pos + len > data.len() {
                    return None;
                }
                if field_number == 3 {
                    // Rebuild the pointer sub-message with new_x / new_y.
                    let sub = &data[pos..pos + len];
                    let rebuilt = rebuild_pointer_sub(sub, new_x, new_y)?;
                    // Write original field tag
                    out.extend_from_slice(&data[tag_start..len_varint_start]);
                    // Write new length as varint
                    write_varint(&mut out, rebuilt.len() as u64);
                    out.extend_from_slice(&rebuilt);
                    pos += len;
                } else {
                    // Copy verbatim
                    out.extend_from_slice(&data[tag_start..pos + len]);
                    pos += len;
                }
            }
            5 => {
                if pos + 4 > data.len() {
                    return None;
                }
                out.extend_from_slice(&data[tag_start..pos + 4]);
                pos += 4;
            }
            _ => return None,
        }
    }
    Some(out)
}

fn rebuild_pointer_sub(sub: &[u8], new_x: u32, new_y: u32) -> Option<Vec<u8>> {
    let mut out: Vec<u8> = Vec::with_capacity(sub.len());
    let mut pos = 0usize;

    while pos < sub.len() {
        let tag_start = pos;
        let tag = read_varint(sub, &mut pos)?;
        let sfn = (tag >> 3) as u32;
        let swt = (tag & 0x07) as u32;
        match swt {
            0 => {
                read_varint(sub, &mut pos)?;
                if sfn == 2 {
                    // Replace x
                    out.extend_from_slice(&sub[tag_start..tag_start + encode_varint_len(tag)]);
                    write_varint(&mut out, new_x as u64);
                } else if sfn == 3 {
                    // Replace y
                    out.extend_from_slice(&sub[tag_start..tag_start + encode_varint_len(tag)]);
                    write_varint(&mut out, new_y as u64);
                } else {
                    out.extend_from_slice(&sub[tag_start..pos]);
                }
            }
            1 => {
                if pos + 8 > sub.len() {
                    return None;
                }
                out.extend_from_slice(&sub[tag_start..pos + 8]);
                pos += 8;
            }
            2 => {
                let len = read_varint(sub, &mut pos)? as usize;
                if pos + len > sub.len() {
                    return None;
                }
                out.extend_from_slice(&sub[tag_start..pos + len]);
                pos += len;
            }
            5 => {
                if pos + 4 > sub.len() {
                    return None;
                }
                out.extend_from_slice(&sub[tag_start..pos + 4]);
                pos += 4;
            }
            _ => return None,
        }
    }
    Some(out)
}

fn write_varint(buf: &mut Vec<u8>, mut v: u64) {
    loop {
        if v < 0x80 {
            buf.push(v as u8);
            return;
        }
        buf.push(((v & 0x7F) as u8) | 0x80);
        v >>= 7;
    }
}

fn encode_varint_len(v: u64) -> usize {
    let mut tmp = Vec::new();
    write_varint(&mut tmp, v);
    tmp.len()
}

// ---------------------------------------------------------------------------
// Video tap — Unix socket sender
// ---------------------------------------------------------------------------

/// A non-blocking sender that forwards video frame `Bytes` to the compositor.
///
/// Uses an mpsc channel internally; the background task drains the channel
/// and writes to the Unix socket.  If the compositor disconnects the task
/// reconnects on the next send attempt.
#[derive(Clone)]
pub struct VideoTap {
    tx: mpsc::Sender<Bytes>,
}

impl VideoTap {
    /// Create a new `VideoTap`.  Spawns a background task that connects to
    /// the compositor socket and forwards video frames.
    pub fn new(socket_path: &str) -> Self {
        let (tx, rx) = mpsc::channel::<Bytes>(64);
        let path = socket_path.to_owned();
        tokio::spawn(video_tap_task(path, rx));
        VideoTap { tx }
    }

    /// Forward a video frame to the compositor.  Non-blocking; drops the
    /// frame if the internal channel is full (back-pressure avoided).
    pub fn send(&self, frame: Bytes) {
        if let Err(e) = self.tx.try_send(frame) {
            debug!("VideoTap: dropped frame ({})", e);
        }
    }
}

async fn video_tap_task(socket_path: String, mut rx: mpsc::Receiver<Bytes>) {
    loop {
        // Wait for at least one frame before attempting to connect.
        let first_frame = match rx.recv().await {
            Some(f) => f,
            None => {
                debug!("VideoTap: sender dropped; task exiting");
                return;
            }
        };

        // Attempt to connect to the compositor socket.
        let mut stream = match UnixStream::connect(&socket_path).await {
            Ok(s) => {
                debug!("VideoTap: connected to {}", socket_path);
                s
            }
            Err(e) => {
                debug!("VideoTap: compositor not available ({}); discarding frames", e);
                // Drain the channel until empty, then wait for the next frame
                // which will trigger another connection attempt.
                while rx.try_recv().is_ok() {}
                continue;
            }
        };

        // Send the frame we buffered while connecting.
        if write_length_prefixed(&mut stream, &first_frame).await.is_err() {
            warn!("VideoTap: write failed on first frame; reconnecting");
            continue;
        }

        // Forward subsequent frames.
        loop {
            match rx.recv().await {
                Some(frame) => {
                    if write_length_prefixed(&mut stream, &frame).await.is_err() {
                        warn!("VideoTap: compositor disconnected; will reconnect");
                        break;
                    }
                }
                None => {
                    debug!("VideoTap: channel closed; exiting");
                    return;
                }
            }
        }
    }
}

/// Write a 4-byte big-endian length prefix followed by the data.
async fn write_length_prefixed(stream: &mut UnixStream, data: &[u8]) -> std::io::Result<()> {
    let len = (data.len() as u32).to_be_bytes();
    stream.write_all(&len).await?;
    stream.write_all(data).await?;
    Ok(())
}

// ---------------------------------------------------------------------------
// CompositorOutput — receives composited frames from the compositor
// ---------------------------------------------------------------------------

/// Shared latest composited frame from the compositor.
///
/// `None` means the compositor has not connected or has not yet sent a frame;
/// in that case the proxy falls back to forwarding the original phone video.
pub type SharedCompositorFrame = Arc<Mutex<Option<Bytes>>>;

pub fn new_shared_compositor_frame() -> SharedCompositorFrame {
    Arc::new(Mutex::new(None))
}

/// Listens on `/tmp/aa-video-out.sock` for composited H.264 frames from the
/// compositor and stores the most recent frame in `shared_frame`.
///
/// Wire format (matching what the compositor sends):
///   [4 bytes big-endian] frame length N
///   [N bytes]            H.264 Annex-B data
///
/// When the compositor disconnects the shared frame is cleared to `None` so
/// the proxy falls back to forwarding the original phone video.
#[derive(Clone)]
pub struct CompositorOutput {
    shared_frame: SharedCompositorFrame,
}

impl CompositorOutput {
    /// Create a new `CompositorOutput`.
    ///
    /// Spawns a background task that binds `/tmp/aa-video-out.sock` and
    /// accepts connections from the compositor.  Only one compositor
    /// connection is served at a time; a new connection replaces the old one.
    pub fn new(socket_path: &str) -> Self {
        let shared_frame = new_shared_compositor_frame();
        let path = socket_path.to_owned();
        let frame_ref = shared_frame.clone();
        tokio::spawn(compositor_output_task(path, frame_ref));
        CompositorOutput { shared_frame }
    }

    /// Take the latest composited frame, if any.
    ///
    /// Returns a clone of the most recently received frame, or `None` if the
    /// compositor is not connected or has not yet sent a frame.
    pub fn latest_frame(&self) -> Option<Bytes> {
        self.shared_frame
            .lock()
            .ok()
            .and_then(|guard| guard.clone())
    }
}

/// Background task: bind the compositor output socket and receive frames.
async fn compositor_output_task(socket_path: String, shared_frame: SharedCompositorFrame) {
    // Remove any stale socket file before binding.
    let _ = std::fs::remove_file(&socket_path);

    let listener = match UnixListener::bind(&socket_path) {
        Ok(l) => {
            log::info!("CompositorOutput: listening on {}", socket_path);
            l
        }
        Err(e) => {
            warn!("CompositorOutput: failed to bind {}: {}", socket_path, e);
            return;
        }
    };

    loop {
        match listener.accept().await {
            Ok((stream, _)) => {
                log::info!("CompositorOutput: compositor connected");
                handle_compositor_connection(stream, shared_frame.clone()).await;
                // Compositor disconnected — clear the shared frame so the proxy
                // falls back to forwarding original phone video.
                if let Ok(mut guard) = shared_frame.lock() {
                    *guard = None;
                }
                log::info!("CompositorOutput: compositor disconnected; falling back to phone video");
            }
            Err(e) => {
                warn!("CompositorOutput: accept error: {}", e);
            }
        }
    }
}

/// Read composited frames from one compositor connection until EOF.
async fn handle_compositor_connection(
    mut stream: UnixStream,
    shared_frame: SharedCompositorFrame,
) {
    loop {
        // Read 4-byte big-endian length prefix.
        let mut len_buf = [0u8; 4];
        match stream.read_exact(&mut len_buf).await {
            Ok(_) => {}
            Err(e) => {
                debug!("CompositorOutput: read length failed: {}", e);
                return;
            }
        }

        let frame_len = u32::from_be_bytes(len_buf) as usize;
        if frame_len == 0 || frame_len > 4 * 1024 * 1024 {
            warn!(
                "CompositorOutput: invalid frame length {} — dropping connection",
                frame_len
            );
            return;
        }

        // Read the H.264 frame data.
        let mut buf = BytesMut::with_capacity(frame_len);
        buf.resize(frame_len, 0);
        match stream.read_exact(&mut buf).await {
            Ok(_) => {}
            Err(e) => {
                debug!("CompositorOutput: read frame failed: {}", e);
                return;
            }
        }

        let frame = buf.freeze();
        debug!("CompositorOutput: received composited frame ({} bytes)", frame.len());

        // Store as the latest frame (overwrite any previous).
        if let Ok(mut guard) = shared_frame.lock() {
            *guard = Some(frame);
        }
    }
}

// ---------------------------------------------------------------------------
// Control socket listener
// ---------------------------------------------------------------------------

/// Spawns a background task that listens on `/tmp/aa-control.sock` for
/// mode-change commands from the compositor.  Updates `shared_mode` in place.
pub fn spawn_control_listener(socket_path: &str, shared_mode: SharedMode) {
    let path = socket_path.to_owned();
    tokio::spawn(control_listener_task(path, shared_mode));
}

async fn control_listener_task(socket_path: String, shared_mode: SharedMode) {
    // Remove stale socket file if present.
    let _ = std::fs::remove_file(&socket_path);

    let listener = match UnixListener::bind(&socket_path) {
        Ok(l) => {
            log::info!("Control socket listening on {}", socket_path);
            l
        }
        Err(e) => {
            warn!("Failed to bind control socket {}: {}", socket_path, e);
            return;
        }
    };

    loop {
        match listener.accept().await {
            Ok((stream, _)) => {
                let mode_ref = shared_mode.clone();
                tokio::spawn(handle_control_connection(stream, mode_ref));
            }
            Err(e) => {
                warn!("Control socket accept error: {}", e);
            }
        }
    }
}

async fn handle_control_connection(stream: UnixStream, shared_mode: SharedMode) {
    use tokio::io::{AsyncBufReadExt, BufReader};

    let reader = BufReader::new(stream);
    let mut lines = reader.lines();

    while let Ok(Some(line)) = lines.next_line().await {
        match parse_control_message(&line) {
            Some(mode) => {
                debug!("Control: new display mode: {:?}", mode);
                if let Ok(mut m) = shared_mode.write() {
                    *m = mode;
                }
            }
            None => {
                warn!("Control: unrecognised message: {}", line);
            }
        }
    }
}

/// Parse a JSON control message into a `DisplayMode`.
///
/// Expected formats:
/// ```json
/// {"mode":"full_aa"}
/// {"mode":"full_cam"}
/// {"mode":"split_aa_cam","aa_region":{"x":0,"y":0,"w":1280,"h":480},"screen":{"w":1280,"h":800}}
/// {"mode":"split_carplay_cam","aa_region":{"x":0,"y":0,"w":1280,"h":480},"screen":{"w":1280,"h":800}}
/// ```
///
/// We intentionally avoid pulling in serde_json to keep dependencies lean;
/// this is a simple hand-rolled parser sufficient for the fixed schema above.
fn parse_control_message(json: &str) -> Option<DisplayMode> {
    // Extract "mode" string value
    let mode_val = extract_string_field(json, "mode")?;
    match mode_val {
        "full_aa" => Some(DisplayMode::FullAa),
        "full_cam" => Some(DisplayMode::FullCam),
        "split_aa_cam" => {
            let (aa_region, screen) = extract_split_fields(json)?;
            Some(DisplayMode::SplitAaCam { aa_region, screen })
        }
        "split_carplay_cam" => {
            let (aa_region, screen) = extract_split_fields(json)?;
            Some(DisplayMode::SplitCarplayCam { aa_region, screen })
        }
        _ => None,
    }
}

fn extract_string_field<'a>(json: &'a str, field: &str) -> Option<&'a str> {
    let needle = format!("\"{}\":", field);
    let start = json.find(&needle)? + needle.len();
    let rest = json[start..].trim_start();
    if !rest.starts_with('"') {
        return None;
    }
    let inner = &rest[1..];
    let end = inner.find('"')?;
    Some(&inner[..end])
}

fn extract_u32_field(json: &str, field: &str) -> Option<u32> {
    let needle = format!("\"{}\":", field);
    let start = json.find(&needle)? + needle.len();
    let rest = json[start..].trim_start();
    let end = rest
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

fn extract_object_str<'a>(json: &'a str, field: &str) -> Option<&'a str> {
    let needle = format!("\"{}\":", field);
    let start = json.find(&needle)? + needle.len();
    let rest = json[start..].trim_start();
    if !rest.starts_with('{') {
        return None;
    }
    // Find matching closing brace
    let mut depth = 0usize;
    for (i, c) in rest.char_indices() {
        match c {
            '{' => depth += 1,
            '}' => {
                depth -= 1;
                if depth == 0 {
                    return Some(&rest[..=i]);
                }
            }
            _ => {}
        }
    }
    None
}

fn extract_split_fields(json: &str) -> Option<(Rect, ScreenSize)> {
    let region_str = extract_object_str(json, "aa_region")?;
    let screen_str = extract_object_str(json, "screen")?;

    let rx = extract_u32_field(region_str, "x")?;
    let ry = extract_u32_field(region_str, "y")?;
    let rw = extract_u32_field(region_str, "w")?;
    let rh = extract_u32_field(region_str, "h")?;

    let sw = extract_u32_field(screen_str, "w")?;
    let sh = extract_u32_field(screen_str, "h")?;

    Some((
        Rect { x: rx, y: ry, w: rw, h: rh },
        ScreenSize { w: sw, h: sh },
    ))
}

// ---------------------------------------------------------------------------
// MitmLayer — the public interface used by proxy.rs
// ---------------------------------------------------------------------------

/// Provides the interception logic for a single proxy session.
///
/// Cloning is cheap (Arc-backed internals).
#[derive(Clone)]
pub struct MitmLayer {
    video_tap: VideoTap,
    compositor_output: CompositorOutput,
    shared_mode: SharedMode,
}

impl MitmLayer {
    pub fn new(
        video_socket: &str,
        compositor_out_socket: &str,
        shared_mode: SharedMode,
    ) -> Self {
        MitmLayer {
            video_tap: VideoTap::new(video_socket),
            compositor_output: CompositorOutput::new(compositor_out_socket),
            shared_mode,
        }
    }

    /// Access the compositor output receiver (for standalone injection mode).
    pub fn compositor_output(&self) -> &CompositorOutput {
        &self.compositor_output
    }

    /// Process a frame that is travelling from the PHONE toward the CAR.
    ///
    /// Returns `Some(frame_to_forward)` — either the original or a modified
    /// copy — or `None` to drop the frame entirely.
    ///
    /// For channel 3 (video):
    ///   1. The raw phone frame is always tapped (sent) to the compositor so
    ///      it can decode, composite, and re-encode it.
    ///   2. If the compositor has returned a composited frame via
    ///      `/tmp/aa-video-out.sock`, that frame REPLACES the original phone
    ///      video going to the car.
    ///   3. If the compositor is not connected (compositor_output has no frame),
    ///      the original phone video is forwarded unchanged (safe fallback).
    pub fn process_phone_to_car(&self, frame: &crate::aap::AapFrame) -> Option<crate::aap::AapFrame> {
        if frame.channel == channel::VIDEO {
            // Always tap raw phone video to compositor for decode/composite.
            self.video_tap.send(frame.payload.clone());

            // If the compositor has produced a composited frame, inject it
            // in place of the original phone video going to the car.
            if let Some(composited) = self.compositor_output.latest_frame() {
                debug!(
                    "MITM: replacing phone video ({} B) with composited frame ({} B)",
                    frame.payload.len(),
                    composited.len()
                );
                return Some(crate::aap::AapFrame {
                    payload: composited,
                    ..frame.clone()
                });
            }

            // Compositor not connected — pass original phone video unchanged.
            debug!("MITM: compositor not connected; forwarding original phone video");
        }
        // Always forward to car (original or composited payload already set above)
        Some(frame.clone())
    }

    /// Process a frame travelling from the CAR toward the PHONE.
    ///
    /// On channel 1 (input/touch) with a touch message type, coordinates
    /// are remapped when split-screen is active.  Returns `None` to drop
    /// the frame (touch landed outside the AA region).
    pub fn process_car_to_phone(&self, frame: &crate::aap::AapFrame) -> Option<crate::aap::AapFrame> {
        if frame.channel == channel::INPUT && frame.msg_type == MSG_TYPE_TOUCH {
            let mode = self.shared_mode.read().ok()?;

            if !mode.aa_visible() {
                // AA not on screen; drop touch entirely
                debug!("MITM: AA not visible; dropping touch event");
                return None;
            }

            if let Some((tx, ty)) = parse_touch_xy(&frame.payload) {
                match remap_touch(tx, ty, &mode) {
                    None => {
                        // Touch outside AA region
                        return None;
                    }
                    Some((new_x, new_y)) if new_x != tx || new_y != ty => {
                        // Remap coordinates in the payload
                        let patched = patch_touch_xy(&frame.payload, new_x, new_y);
                        return Some(crate::aap::AapFrame {
                            payload: patched,
                            ..frame.clone()
                        });
                    }
                    Some(_) => {
                        // Coordinates unchanged — pass original frame
                    }
                }
            }
        }
        Some(frame.clone())
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_remap_full_aa() {
        let mode = DisplayMode::FullAa;
        assert_eq!(remap_touch(640, 400, &mode), Some((640, 400)));
    }

    #[test]
    fn test_remap_split_inside() {
        let mode = DisplayMode::SplitAaCam {
            aa_region: Rect { x: 0, y: 0, w: 1280, h: 480 },
            screen: ScreenSize { w: 1280, h: 800 },
        };
        // Touch in the top half (AA region)
        assert_eq!(remap_touch(640, 200, &mode), Some((640, 200)));
    }

    #[test]
    fn test_remap_split_outside() {
        let mode = DisplayMode::SplitAaCam {
            aa_region: Rect { x: 0, y: 0, w: 1280, h: 480 },
            screen: ScreenSize { w: 1280, h: 800 },
        };
        // Touch in the bottom half (cam region) — should be discarded
        assert_eq!(remap_touch(640, 600, &mode), None);
    }

    #[test]
    fn test_remap_full_cam() {
        let mode = DisplayMode::FullCam;
        // aa_region() returns None for full_cam, so remap_touch returns Some((x,y))
        // but aa_visible() is false so proxy layer drops it upstream.
        assert_eq!(remap_touch(640, 400, &mode), Some((640, 400)));
        assert!(!mode.aa_visible());
    }

    #[test]
    fn test_parse_control_full_aa() {
        let msg = r#"{"mode":"full_aa"}"#;
        assert_eq!(parse_control_message(msg), Some(DisplayMode::FullAa));
    }

    #[test]
    fn test_parse_control_split() {
        let msg = r#"{"mode":"split_aa_cam","aa_region":{"x":0,"y":0,"w":1280,"h":480},"screen":{"w":1280,"h":800}}"#;
        let result = parse_control_message(msg);
        assert!(matches!(result, Some(DisplayMode::SplitAaCam { .. })));
        if let Some(DisplayMode::SplitAaCam { aa_region, screen }) = result {
            assert_eq!(aa_region.w, 1280);
            assert_eq!(aa_region.h, 480);
            assert_eq!(screen.w, 1280);
            assert_eq!(screen.h, 800);
        }
    }
}

/*
 * aa_emulator.c — Android Auto protocol emulator for CarPlay-only mode
 *
 * AAP wire format recap (from aa_protocol.h):
 *
 *   Header (6 bytes):
 *     [0]   channel id
 *     [1]   flags  (SINGLE=0x08, FIRST=0x09, CONT=0x0A, LAST=0x0B)
 *     [2-3] payload length (big-endian uint16)
 *     [4-5] total message length (big-endian uint16, first frag only)
 *
 *   Control-channel (ch=0) message structure:
 *     [0-1] message type (big-endian uint16)
 *     [2..] TLV payload
 *
 * Message types (partial list, from open-source AA research):
 *   0x0001  VERSION_REQUEST     car → phone
 *   0x0002  VERSION_RESPONSE    phone → car
 *   0x0003  SSL_HANDSHAKE       (skipped — no SSL in emulator)
 *   0x0005  AUTH_COMPLETE       phone → car  (we fake this)
 *   0x000D  SERVICE_DISCOVERY_REQUEST  car → phone
 *   0x000E  SERVICE_DISCOVERY_RESPONSE phone → car
 *   0x000F  CHANNEL_OPEN_REQUEST  car → phone
 *   0x0010  CHANNEL_OPEN_RESPONSE phone → car
 *   0x000B  PING_REQUEST        car → phone
 *   0x000C  PING_RESPONSE       phone → car
 *
 * Video channel (ch=3):
 *   0x0001  MEDIA_SETUP_REQUEST
 *   0x0002  MEDIA_SETUP_RESPONSE
 *   0x0004  VIDEO_FOCUS_REQUEST
 *   0x0005  MediaDataWithTimestamp  (what we send)
 */
#include "aa_emulator.h"
#include "aa_protocol.h"
#include "fragmentation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>

/* ---- Internal state ---- */

static int              g_car_fd    = -1;
static aa_emu_touch_cb  g_touch_cb  = NULL;
static pthread_mutex_t  g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* Fragmentation context for outgoing video (channel 3) */
static frag_ctx_t       g_frag_out;

/* ---- Low-level send helpers ---- */

/*
 * Build a 6-byte AAP header at `out`.
 * payload_len:  size of this fragment's payload
 * total_len:    total message size (same as payload_len for SINGLE frames)
 */
static void build_header(uint8_t *out, uint8_t channel, uint8_t flags,
                          uint16_t payload_len, uint16_t total_len)
{
    out[0] = channel;
    out[1] = flags;
    out[2] = (uint8_t)(payload_len >> 8);
    out[3] = (uint8_t)(payload_len & 0xFF);
    out[4] = (uint8_t)(total_len  >> 8);
    out[5] = (uint8_t)(total_len  & 0xFF);
}

/* Send a complete AAP frame (header already built in buf[0..5]) */
static int raw_send(const uint8_t *header, const uint8_t *payload, size_t plen)
{
    if (g_car_fd < 0)
        return -1;

    /* Use iovec for two-part send to avoid extra copy */
    struct msghdr msg;
    struct iovec iov[2];

    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = (void *)header;
    iov[0].iov_len  = AAP_HEADER_SIZE;
    iov[1].iov_base = (void *)payload;
    iov[1].iov_len  = plen;
    msg.msg_iov     = iov;
    msg.msg_iovlen  = 2;

    ssize_t sent = sendmsg(g_car_fd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        perror("aa_emu: sendmsg");
        return -1;
    }
    return 0;
}

/*
 * Send a control-channel (ch=0) message.
 * msg_type is the 2-byte type; payload follows (may be NULL / len=0).
 */
static int ctrl_send(uint16_t msg_type, const uint8_t *payload, size_t plen)
{
    /* Control messages: 2-byte type + optional TLV payload */
    uint8_t buf[2048];
    size_t  total = 2 + plen;

    if (total > sizeof(buf)) return -1;

    buf[0] = (uint8_t)(msg_type >> 8);
    buf[1] = (uint8_t)(msg_type & 0xFF);
    if (payload && plen)
        memcpy(buf + 2, payload, plen);

    uint8_t hdr[AAP_HEADER_SIZE];
    build_header(hdr, AAP_CH_CONTROL, AAP_FLAG_SINGLE,
                 (uint16_t)total, (uint16_t)total);

    return raw_send(hdr, buf, total);
}

/* ---- Handshake sequence ---- */

/*
 * VERSION_RESPONSE: major=1, minor=5
 * Wire: uint16 major, uint16 minor, uint32 max_msg_size
 */
static void send_version_response(void)
{
    uint8_t payload[8];
    /* major = 1 */
    payload[0] = 0x00; payload[1] = 0x01;
    /* minor = 5 */
    payload[2] = 0x00; payload[3] = 0x05;
    /* max_msg_size = 16384 */
    payload[4] = 0x00; payload[5] = 0x00;
    payload[6] = 0x40; payload[7] = 0x00;

    ctrl_send(0x0002, payload, sizeof(payload));
    printf("aa_emu: sent VERSION_RESPONSE (1.5)\n");
}

/*
 * AUTH_COMPLETE: tells car that authentication succeeded.
 * In a real AA session this follows SSL+auth chip; we skip straight to this.
 * Payload: status=0 (OK) as uint8.
 */
static void send_auth_complete(void)
{
    uint8_t payload[1] = { 0x00 }; /* status OK */
    ctrl_send(0x0005, payload, 1);
    printf("aa_emu: sent AUTH_COMPLETE\n");
}

/*
 * SERVICE_DISCOVERY_RESPONSE: advertise video output service only.
 *
 * Minimal TLV encoding:
 *   tag=0x01 (services list), each service:
 *     tag=0x01 channel_id  varint
 *     tag=0x02 service_type varint  (VIDEO_OUTPUT = 11)
 *     tag=0x03 service_name string
 *
 * We use a pre-built binary blob that announces one service: video output
 * on channel 3.  This is derived from Wireshark captures of real AA dongles.
 */
static void send_service_discovery_response(void)
{
    /*
     * Minimal protobuf-compatible TLV for one video service:
     *   field 1 (services), embedded message:
     *     field 1 (channel_id) = 3
     *     field 2 (type)       = 11 (VIDEO_OUTPUT)
     */
    static const uint8_t discovery_blob[] = {
        0x0A,       /* field 1, wire type 2 (length-delimited) */
        0x04,       /* length 4 */
        0x08, 0x03, /* field 1 (channel_id), varint: 3 */
        0x10, 0x0B, /* field 2 (type),       varint: 11 */
    };
    ctrl_send(0x000E, discovery_blob, sizeof(discovery_blob));
    printf("aa_emu: sent SERVICE_DISCOVERY_RESPONSE\n");
}

/*
 * CHANNEL_OPEN_RESPONSE: acknowledge with status OK.
 * payload: uint8 status=0.
 */
static void send_channel_open_response(uint8_t channel_id)
{
    (void)channel_id;
    uint8_t payload[1] = { 0x00 }; /* status OK */
    ctrl_send(0x0010, payload, 1);
    printf("aa_emu: sent CHANNEL_OPEN_RESPONSE for ch%u\n", channel_id);
}

/*
 * PING_RESPONSE: echo back the ping payload.
 */
static void send_ping_response(const uint8_t *ping_payload, size_t plen)
{
    ctrl_send(0x000C, ping_payload, plen);
}

/*
 * VIDEO_MEDIA_SETUP_RESPONSE: acknowledge video channel setup.
 * Payload: uint8 status=0.
 */
static void send_video_setup_response(void)
{
    uint8_t buf[3];
    buf[0] = 0x00; buf[1] = 0x02; /* msg type: MEDIA_SETUP_RESPONSE */
    buf[2] = 0x00;                /* status OK */

    uint8_t hdr[AAP_HEADER_SIZE];
    build_header(hdr, AAP_CH_VIDEO, AAP_FLAG_SINGLE, 3, 3);
    raw_send(hdr, buf, 3);
    printf("aa_emu: sent VIDEO_SETUP_RESPONSE\n");
}

/* ---- Video send with fragmentation ---- */

/*
 * Callback used by frag_split() to send each fragment.
 * user_ctx = pointer to the pre-built video payload buffer.
 * We rebuild the header for each fragment here.
 */
typedef struct {
    size_t total_len; /* full unfragmented message length */
} send_frag_ctx_t;

static void send_fragment_cb(uint8_t channel, uint8_t flags,
                              const uint8_t *data, size_t len,
                              void *user_ctx)
{
    send_frag_ctx_t *sc = (send_frag_ctx_t *)user_ctx;
    uint8_t hdr[AAP_HEADER_SIZE];

    /*
     * For FIRST fragment, total_len carries the full message size so the
     * receiver can pre-allocate.  For other fragments, total_len == payload_len.
     */
    uint16_t total = (flags == FRAG_FLAG_FIRST)
                     ? (uint16_t)(sc->total_len & 0xFFFF)
                     : (uint16_t)(len & 0xFFFF);

    build_header(hdr, channel, flags, (uint16_t)(len & 0xFFFF), total);
    raw_send(hdr, data, len);
}

/* ---- Control message dispatch ---- */

static void handle_control_message(const uint8_t *payload, size_t plen)
{
    if (plen < 2) return;

    uint16_t msg_type = ((uint16_t)payload[0] << 8) | payload[1];

    switch (msg_type) {
    case 0x0001: /* VERSION_REQUEST */
        send_version_response();
        /* After version negotiation, skip SSL — send AUTH_COMPLETE directly.
         * Real dongles do full SSL here but the car accepts auth_complete
         * after version exchange in practice (reference: nisargjhaveri/WAAD). */
        send_auth_complete();
        break;

    case 0x000D: /* SERVICE_DISCOVERY_REQUEST */
        send_service_discovery_response();
        break;

    case 0x000F: /* CHANNEL_OPEN_REQUEST */
        /* Payload[2] = channel_id in simple cases */
        send_channel_open_response(plen > 2 ? payload[2] : 0);
        break;

    case 0x000B: /* PING_REQUEST */
        send_ping_response(payload + 2, plen - 2);
        break;

    default:
        /* Silently ignore unrecognised control messages */
        break;
    }
}

static void handle_video_message(const uint8_t *payload, size_t plen)
{
    if (plen < 2) return;
    uint16_t msg_type = ((uint16_t)payload[0] << 8) | payload[1];

    if (msg_type == 0x0001) { /* MEDIA_SETUP_REQUEST */
        send_video_setup_response();
    }
    /* Other video channel messages (focus requests, etc.) are ignored */
}

/* ---- Public API ---- */

int aa_emu_init(int car_fd, aa_emu_touch_cb touch_cb)
{
    if (car_fd < 0)
        return -1;

    g_car_fd   = car_fd;
    g_touch_cb = touch_cb;
    frag_ctx_init(&g_frag_out);

    /*
     * Initiate handshake proactively.
     * Some cars wait for VERSION_REQUEST before sending it; others expect
     * the dongle to send VERSION_RESPONSE after an initial challenge.
     * We send VERSION_RESPONSE + AUTH_COMPLETE immediately; the car will
     * respond with SERVICE_DISCOVERY_REQUEST which we handle in
     * aa_emu_handle_car_message().
     */
    send_version_response();
    send_auth_complete();

    printf("aa_emu: initialised on fd=%d\n", car_fd);
    return 0;
}

int aa_emu_send_video(const uint8_t *h264, size_t len, uint64_t ts_ns)
{
    if (g_car_fd < 0 || !h264 || len == 0)
        return -1;

    /*
     * Build the AAP video payload:
     *   [0-1]  msg type  = 0x0005 (MediaDataWithTimestamp)
     *   [2-9]  timestamp = ts_ns (big-endian uint64)
     *   [10..] H.264 NAL data
     */
    size_t payload_size = 10 + len;
    uint8_t *payload = malloc(payload_size);
    if (!payload) return -1;

    payload[0] = 0x00; payload[1] = 0x05;
    for (int i = 7; i >= 0; i--)
        payload[2 + (7 - i)] = (uint8_t)(ts_ns >> (i * 8));
    memcpy(payload + 10, h264, len);

    pthread_mutex_lock(&g_send_lock);

    send_frag_ctx_t sc = { .total_len = payload_size };
    frag_split(AAP_CH_VIDEO, payload, payload_size,
               send_fragment_cb, &sc);

    pthread_mutex_unlock(&g_send_lock);
    free(payload);
    return 0;
}

void aa_emu_handle_car_message(const uint8_t *frame, size_t len)
{
    if (!frame || len < AAP_HEADER_SIZE)
        return;

    aap_frame_t f;
    if (!aap_parse_header(frame, len, &f))
        return;

    switch (f.channel) {
    case AAP_CH_CONTROL:
        handle_control_message(f.payload, f.payload_len);
        break;

    case AAP_CH_INPUT:
        /* Touch event from car — forward to CarPlay touch handler */
        if (g_touch_cb && f.payload && f.payload_len > 0)
            g_touch_cb(f.payload, f.payload_len);
        break;

    case AAP_CH_VIDEO:
        /* Car sending something on the video channel (e.g. setup request) */
        handle_video_message(f.payload, f.payload_len);
        break;

    default:
        /* Sensor, audio, nav — ignore in emulator */
        break;
    }
}

void aa_emu_destroy(void)
{
    frag_ctx_destroy(&g_frag_out);

    if (g_car_fd >= 0) {
        close(g_car_fd);
        g_car_fd = -1;
    }
    g_touch_cb = NULL;
    printf("aa_emu: destroyed\n");
}

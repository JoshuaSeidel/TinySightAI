/*
 * airplay_rtsp.c — RTSP Session Handler for AirPlay CarPlay
 *
 * Implements the RTSP control plane for AirPlay 2 screen mirroring.
 *
 * Flow:
 *   POST /pair-setup  (M1/M2, then M3/M4)
 *   POST /pair-verify (M1/M2, then M3/M4)
 *   POST /fp-setup    (stage 1, 2, 3)
 *   OPTIONS *
 *   POST /info
 *   ANNOUNCE          (SDP)
 *   SETUP             (video mirror + audio streams)
 *   RECORD
 *   GET_PARAMETER     (keep-alive)
 *   SET_PARAMETER     (volume, HID)
 *   TEARDOWN
 */

#include "airplay_rtsp.h"
#include "airplay_pair.h"
#include "airplay_fairplay.h"
#include "airplay_mirror.h"
#include "airplay_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* -----------------------------------------------------------------------
 * Internal request/response structures
 * ----------------------------------------------------------------------- */

#define MAX_HEADERS 64

typedef struct {
    char method[32];
    char uri[256];
    char version[16];

    /* Headers */
    char header_names [MAX_HEADERS][64];
    char header_values[MAX_HEADERS][256];
    int  header_count;

    /* Body */
    uint8_t *body;
    size_t   body_len;
    int      cseq;
} rtsp_request_t;

/* -----------------------------------------------------------------------
 * Binary plist helpers (minimal — just enough for /info response)
 *
 * We take the easy approach: return a pre-built /info response plist
 * as a hex blob. This is the same approach used by RPiPlay and shairport-sync.
 *
 * The /info response tells the iPhone:
 *   - device model (AppleTV3,2 for widest compatibility)
 *   - features bitmask (screen mirroring + audio)
 *   - MAC address
 *   - server public key
 *   - protocol version
 * ----------------------------------------------------------------------- */

/*
 * Build a minimal binary plist for the /info response.
 * We use a text plist format encoded as XML then return it as
 * Content-Type: application/x-apple-binary-plist.
 *
 * For simplicity we return it as text/x-apple-plist (text format)
 * which iOS also accepts for /info.
 */
static int build_info_plist(const char *mac, const char *name,
                              const uint8_t ed25519_pub[32],
                              char *out, size_t out_cap)
{
    /* Convert Ed25519 pub key to hex string for pk field */
    char pk_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(pk_hex + i*2, 3, "%02x", ed25519_pub[i]);
    }

    /*
     * features bitmap:
     *   0x5A7FFFF7 = screen mirroring, audio, HID, etc.
     *   See AirPlay2 feature flag definitions in RPiPlay / homebridge-airplay2
     */
    int n = snprintf(out, out_cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>deviceid</key><string>%s</string>\n"
        "  <key>features</key><integer>1496777203</integer>\n"
        "  <key>model</key><string>AppleTV3,2</string>\n"
        "  <key>name</key><string>%s</string>\n"
        "  <key>pi</key><string>b08f5a79-db29-4384-b456-a4784d9e6055</string>\n"
        "  <key>pk</key><string>%s</string>\n"
        "  <key>protovers</key><string>1.1</string>\n"
        "  <key>srcvers</key><string>220.68</string>\n"
        "  <key>vv</key><integer>2</integer>\n"
        "</dict>\n"
        "</plist>\n",
        mac, name, pk_hex);

    return (n > 0 && (size_t)n < out_cap) ? n : -1;
}

/* -----------------------------------------------------------------------
 * HTTP/RTSP request parser
 * ----------------------------------------------------------------------- */

static const char *header_get(const rtsp_request_t *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->header_names[i], name) == 0)
            return req->header_values[i];
    }
    return NULL;
}

/*
 * Read one complete RTSP/HTTP request from fd.
 * Allocates req->body on success; caller must free().
 * Returns 0 on success, -1 on error/EOF.
 */
static int rtsp_read_request(int fd, rtsp_request_t *req)
{
    memset(req, 0, sizeof(*req));
    req->cseq = -1;

    /* Read headers in bulk, scanning for the \r\n\r\n terminator */
    static __thread char hdr_buf[16384];
    int hdr_len = 0;
    int hdr_end = -1;

    while (hdr_len < (int)sizeof(hdr_buf) - 1) {
        ssize_t n = recv(fd, hdr_buf + hdr_len, sizeof(hdr_buf) - 1 - hdr_len, 0);
        if (n <= 0) return -1;
        hdr_len += (int)n;

        /* Scan for \r\n\r\n in the buffer */
        int scan_start = hdr_len - (int)n - 3;
        if (scan_start < 0) scan_start = 0;
        for (int i = scan_start; i <= hdr_len - 4; i++) {
            if (hdr_buf[i] == '\r' && hdr_buf[i+1] == '\n' &&
                hdr_buf[i+2] == '\r' && hdr_buf[i+3] == '\n') {
                hdr_end = i + 4;
                break;
            }
        }
        if (hdr_end >= 0) break;
    }
    if (hdr_end < 0) return -1;

    /* Any bytes past the header end are body data we already received */
    int extra_body_bytes = hdr_len - hdr_end;
    /* NUL-terminate headers without clobbering body data */
    char saved_byte = hdr_buf[hdr_end];
    hdr_buf[hdr_end] = '\0';

    /* Parse request line */
    char *p = hdr_buf;
    char *line_end = strstr(p, "\r\n");
    if (!line_end) return -1;
    *line_end = '\0';

    if (sscanf(p, "%31s %255s %15s", req->method, req->uri, req->version) != 3) {
        return -1;
    }
    p = line_end + 2;

    /* Parse headers */
    while (*p && *p != '\r') {
        line_end = strstr(p, "\r\n");
        if (!line_end) break;
        *line_end = '\0';

        char *colon = strchr(p, ':');
        if (colon && req->header_count < MAX_HEADERS) {
            *colon = '\0';
            char *val = colon + 1;
            while (*val == ' ') val++;
            strncpy(req->header_names [req->header_count], p,   sizeof(req->header_names[0])  - 1);
            strncpy(req->header_values[req->header_count], val, sizeof(req->header_values[0]) - 1);
            if (strcasecmp(p, "CSeq") == 0) {
                req->cseq = atoi(val);
            }
            req->header_count++;
        }
        p = line_end + 2;
    }

    /* Read body if Content-Length present */
    const char *cl = header_get(req, "Content-Length");
    if (cl) {
        req->body_len = (size_t)atol(cl);
        if (req->body_len > 0 && req->body_len <= RTSP_MAX_REQUEST_BYTES) {
            req->body = malloc(req->body_len);
            if (!req->body) return -1;

            /* Copy any body bytes we already received during header read */
            size_t done = 0;
            hdr_buf[hdr_end] = saved_byte; /* restore before copying */
            if (extra_body_bytes > 0) {
                size_t copy = (size_t)extra_body_bytes < req->body_len
                              ? (size_t)extra_body_bytes : req->body_len;
                memcpy(req->body, hdr_buf + hdr_end, copy);
                done = copy;
            }

            while (done < req->body_len) {
                ssize_t n = recv(fd, req->body + done, req->body_len - done, 0);
                if (n <= 0) { free(req->body); req->body = NULL; return -1; }
                done += n;
            }
        }
    }

    return 0;
}

static void rtsp_request_free(rtsp_request_t *req)
{
    free(req->body);
    req->body = NULL;
}

/* -----------------------------------------------------------------------
 * RTSP response builder
 * ----------------------------------------------------------------------- */

static int rtsp_send_response(int fd, int cseq, int status, const char *reason,
                               const char *content_type,
                               const uint8_t *body, size_t body_len)
{
    char hdr[1024];
    int hdr_len = 0;

    hdr_len += snprintf(hdr + hdr_len, sizeof(hdr) - hdr_len,
                         "RTSP/1.0 %d %s\r\n", status, reason);
    if (cseq >= 0) {
        hdr_len += snprintf(hdr + hdr_len, sizeof(hdr) - hdr_len,
                             "CSeq: %d\r\n", cseq);
    }
    hdr_len += snprintf(hdr + hdr_len, sizeof(hdr) - hdr_len,
                         "Server: AirTunes/220.68\r\n");
    if (content_type && body_len > 0) {
        hdr_len += snprintf(hdr + hdr_len, sizeof(hdr) - hdr_len,
                             "Content-Type: %s\r\n", content_type);
        hdr_len += snprintf(hdr + hdr_len, sizeof(hdr) - hdr_len,
                             "Content-Length: %zu\r\n", body_len);
    } else {
        hdr_len += snprintf(hdr + hdr_len, sizeof(hdr) - hdr_len,
                             "Content-Length: 0\r\n");
    }
    hdr_len += snprintf(hdr + hdr_len, sizeof(hdr) - hdr_len, "\r\n");

    /* Send header */
    if (send(fd, hdr, hdr_len, MSG_NOSIGNAL) < 0) return -1;

    /* Send body */
    if (body && body_len > 0) {
        size_t sent = 0;
        while (sent < body_len) {
            ssize_t n = send(fd, body + sent, body_len - sent, MSG_NOSIGNAL);
            if (n < 0) return -1;
            sent += n;
        }
    }
    return 0;
}

/* Convenience: send 200 OK with no body */
static int rtsp_ok(int fd, int cseq)
{
    return rtsp_send_response(fd, cseq, 200, "OK", NULL, NULL, 0);
}

/* -----------------------------------------------------------------------
 * SDP parser (minimal — just look for the stream control URIs)
 * ----------------------------------------------------------------------- */

/*
 * Extract the stream type (audio or video) from an SDP.
 * We look for "m=video" and "m=audio" lines to know what SETUP will configure.
 * Not a full SDP parser — just enough for our purposes.
 */
static bool sdp_has_video(const char *sdp)
{
    return (strstr(sdp, "m=video") != NULL);
}

static bool sdp_has_audio(const char *sdp)
{
    return (strstr(sdp, "m=audio") != NULL);
}

/* -----------------------------------------------------------------------
 * Request handlers
 * ----------------------------------------------------------------------- */

/*
 * OPTIONS * → return all supported methods.
 */
static int handle_options(int fd, int cseq)
{
    char body[] =
        "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, "
        "OPTIONS, GET_PARAMETER, SET_PARAMETER, POST, GET\r\n";
    return rtsp_send_response(fd, cseq, 200, "OK",
                               "text/parameters",
                               (const uint8_t *)body, strlen(body));
}

/*
 * POST /info → return device capabilities as XML plist.
 */
static int handle_info(airplay_rtsp_session_t *sess, int fd, int cseq)
{
    char plist[4096];
    int n = build_info_plist(sess->device_mac, sess->device_name,
                              sess->pair.ed25519_pub, plist, sizeof(plist));
    if (n < 0) return rtsp_ok(fd, cseq);

    return rtsp_send_response(fd, cseq, 200, "OK",
                               "application/x-apple-plist",
                               (const uint8_t *)plist, n);
}

/*
 * POST /pair-setup → delegate to pair module.
 */
static int handle_pair_setup(airplay_rtsp_session_t *sess,
                               int fd, const rtsp_request_t *req)
{
    uint8_t out[4096];
    size_t  out_len = 0;

    if (!req->body || req->body_len == 0) {
        return rtsp_send_response(fd, req->cseq, 400, "Bad Request", NULL, NULL, 0);
    }

    if (pair_setup_process(&sess->pair, req->body, req->body_len,
                            out, &out_len) < 0) {
        fprintf(stderr, "rtsp: pair-setup failed\n");
        return rtsp_send_response(fd, req->cseq, 500, "Internal Server Error",
                                   NULL, NULL, 0);
    }

    if (pair_get_state(&sess->pair) == PAIR_STATE_SETUP_DONE) {
        sess->state = RTSP_STATE_PAIRED;
    }

    return rtsp_send_response(fd, req->cseq, 200, "OK",
                               "application/x-apple-binary-plist",
                               out, out_len);
}

/*
 * POST /pair-verify → delegate to pair module.
 */
static int handle_pair_verify(airplay_rtsp_session_t *sess,
                                int fd, const rtsp_request_t *req)
{
    uint8_t out[4096];
    size_t  out_len = 0;

    if (!req->body || req->body_len == 0) {
        return rtsp_send_response(fd, req->cseq, 400, "Bad Request", NULL, NULL, 0);
    }

    if (pair_verify_process(&sess->pair, req->body, req->body_len,
                             out, &out_len) < 0) {
        fprintf(stderr, "rtsp: pair-verify failed\n");
        return rtsp_send_response(fd, req->cseq, 500, "Internal Server Error",
                                   NULL, NULL, 0);
    }

    if (pair_get_state(&sess->pair) == PAIR_STATE_VERIFIED) {
        sess->state = RTSP_STATE_VERIFIED;
        printf("rtsp: pair-verify complete — session verified\n");
    }

    return rtsp_send_response(fd, req->cseq, 200, "OK",
                               "application/x-apple-binary-plist",
                               out, out_len);
}

/*
 * POST /fp-setup → FairPlay handshake.
 */
static int handle_fp_setup(airplay_rtsp_session_t *sess,
                             int fd, const rtsp_request_t *req)
{
    if (!req->body || req->body_len == 0) {
        return rtsp_send_response(fd, req->cseq, 400, "Bad Request", NULL, NULL, 0);
    }

    uint8_t out[256];
    size_t  out_len = 0;

    if (fairplay_process(&sess->fairplay, req->body, req->body_len,
                          out, &out_len) < 0) {
        fprintf(stderr, "rtsp: fp-setup stage failed\n");
        return rtsp_send_response(fd, req->cseq, 500, "Internal Server Error",
                                   NULL, NULL, 0);
    }

    if (fairplay_get_state(&sess->fairplay) == FP_STATE_DONE) {
        sess->state = RTSP_STATE_FP_DONE;
        /*
         * Retrieve the AES-128-CTR key and IV derived during the FairPlay
         * handshake.  The key is produced at stage 3; the IV (server_iv) is
         * the random 16-byte value generated at stage 1 and exchanged with
         * the iPhone as part of the stage 1 response blob.
         *
         * Both are now available in the fairplay context — wire them directly
         * to the mirror parser so it can decrypt FairPlay-encrypted H.264 NALs
         * as soon as the mirror stream connection arrives.
         */
        uint8_t aes_key[16];
        uint8_t aes_iv[16];
        fairplay_get_aes_key(&sess->fairplay, aes_key);
        fairplay_get_iv(&sess->fairplay, aes_iv);
        airplay_mirror_set_fairplay(&sess->mirror, aes_key, aes_iv);
        printf("rtsp: FairPlay handshake complete — AES key and IV wired to mirror context\n");
    }

    const char *ct = out_len > 0 ? "application/octet-stream" : NULL;
    return rtsp_send_response(fd, req->cseq, 200, "OK", ct, out, out_len);
}

/*
 * ANNOUNCE → parse SDP, note video/audio presence.
 */
static int handle_announce(airplay_rtsp_session_t *sess,
                             int fd, const rtsp_request_t *req)
{
    if (req->body && req->body_len > 0) {
        /* SDP is text — safe to treat as string (add NUL) */
        char *sdp = malloc(req->body_len + 1);
        if (sdp) {
            memcpy(sdp, req->body, req->body_len);
            sdp[req->body_len] = '\0';
            sess->streams.video_active = sdp_has_video(sdp);
            sess->streams.audio_active = sdp_has_audio(sdp);
            printf("rtsp: ANNOUNCE — video=%d audio=%d\n",
                   sess->streams.video_active, sess->streams.audio_active);
            free(sdp);
        }
    }
    sess->state = RTSP_STATE_ANNOUNCED;
    return rtsp_ok(fd, req->cseq);
}

/*
 * SETUP → allocate ports, return Transport header.
 *
 * The URI in SETUP tells us which stream to configure:
 *   /stream=0   or  /video   → mirror/video stream
 *   /stream=1   or  /audio   → audio stream
 *
 * We respond with the server's port allocations.
 * AirPlay typically uses TCP for the mirror stream and UDP for audio.
 */
static int handle_setup(airplay_rtsp_session_t *sess,
                          int fd, const rtsp_request_t *req)
{
    const char *transport = header_get(req, "Transport");

    /* Determine stream type from URI */
    bool is_video = (strstr(req->uri, "video") != NULL ||
                     strstr(req->uri, "stream=0") != NULL ||
                     strstr(req->uri, "streamid=0") != NULL);
    bool is_audio = (strstr(req->uri, "audio") != NULL ||
                     strstr(req->uri, "stream=1") != NULL ||
                     strstr(req->uri, "streamid=1") != NULL);

    /* If URI is ambiguous, use the Transport header to guess */
    if (!is_video && !is_audio) {
        if (transport && strstr(transport, "RTP/AVP")) {
            is_audio = true;  /* RTP/AVP is typically audio */
        } else {
            is_video = true;  /* default to video */
        }
    }

    char resp_transport[512];

    if (is_video) {
        sess->streams.video_port = AIRPLAY_MIRROR_PORT_DEFAULT;
        sess->streams.video_tcp  = true;
        sess->streams.video_active = true;

        /* Return TCP mirror stream port */
        snprintf(resp_transport, sizeof(resp_transport),
                 "RTP/AVP/TCP;unicast;interleaved=0-1;server_port=%u",
                 sess->streams.video_port);
        printf("rtsp: SETUP video → port %u (TCP)\n", sess->streams.video_port);

    } else if (is_audio) {
        sess->streams.audio_data_port    = AIRPLAY_AUDIO_DATA_PORT;
        sess->streams.audio_control_port = AIRPLAY_AUDIO_CONTROL_PORT;
        sess->streams.audio_active = true;

        snprintf(resp_transport, sizeof(resp_transport),
                 "RTP/AVP/UDP;unicast;"
                 "server_port=%u;control_port=%u",
                 sess->streams.audio_data_port,
                 sess->streams.audio_control_port);
        printf("rtsp: SETUP audio → data port %u, control port %u\n",
               sess->streams.audio_data_port,
               sess->streams.audio_control_port);
    } else {
        return rtsp_send_response(fd, req->cseq, 461, "Unsupported Transport",
                                   NULL, NULL, 0);
    }

    /* Build response with Transport and Session headers */
    char resp_hdr[1024];
    int n = snprintf(resp_hdr, sizeof(resp_hdr),
        "RTSP/1.0 200 OK\r\n"
        "CSeq: %d\r\n"
        "Server: AirTunes/220.68\r\n"
        "Session: DEADBEEF1234;timeout=90\r\n"
        "Transport: %s\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        req->cseq, resp_transport);

    if (send(fd, resp_hdr, n, MSG_NOSIGNAL) < 0) return -1;

    sess->state = RTSP_STATE_SETUP;
    return 0;
}

/*
 * RECORD → acknowledge, begin streaming.
 */
static int handle_record(airplay_rtsp_session_t *sess,
                          int fd, const rtsp_request_t *req)
{
    sess->state = RTSP_STATE_RECORDING;
    printf("rtsp: RECORD — streaming started\n");
    return rtsp_ok(fd, req->cseq);
}

/*
 * GET_PARAMETER → keep-alive or parameter query.
 * For empty body just return 200 OK.
 */
static int handle_get_parameter(int fd, const rtsp_request_t *req)
{
    /* If body is empty, it's a keep-alive ping */
    if (!req->body || req->body_len == 0) {
        return rtsp_ok(fd, req->cseq);
    }

    /* Otherwise echo back an empty response (we don't support parameter queries) */
    return rtsp_ok(fd, req->cseq);
}

/*
 * SET_PARAMETER → volume, HID events, etc.
 */
static int handle_set_parameter(airplay_rtsp_session_t *sess,
                                  int fd, const rtsp_request_t *req)
{
    const char *ct = header_get(req, "Content-Type");

    if (ct && strstr(ct, "text/parameters") && req->body && req->body_len > 0) {
        char *params = malloc(req->body_len + 1);
        if (params) {
            memcpy(params, req->body, req->body_len);
            params[req->body_len] = '\0';
            /* Parse "volume: -30.0\r\n" etc. */
            if (strncmp(params, "volume:", 7) == 0) {
                float vol = atof(params + 7);
                printf("rtsp: SET_PARAMETER volume=%.1f\n", vol);
                /* Forward to audio handler */
                airplay_audio_set_volume(vol);
            }
            free(params);
        }
    }

    (void)sess;
    return rtsp_ok(fd, req->cseq);
}

/*
 * TEARDOWN → end session.
 */
static int handle_teardown(airplay_rtsp_session_t *sess,
                             int fd, const rtsp_request_t *req)
{
    sess->state = RTSP_STATE_TEARDOWN;
    printf("rtsp: TEARDOWN received\n");
    rtsp_ok(fd, req->cseq);
    return -1; /* Signal to close the connection */
}

/* -----------------------------------------------------------------------
 * POST /feedback → timing/latency feedback (no-op for basic impl)
 * ----------------------------------------------------------------------- */
static int handle_feedback(int fd, const rtsp_request_t *req)
{
    return rtsp_ok(fd, req->cseq);
}

/* -----------------------------------------------------------------------
 * Main dispatch
 * ----------------------------------------------------------------------- */

static int rtsp_dispatch(airplay_rtsp_session_t *sess,
                          int fd, const rtsp_request_t *req)
{
    printf("rtsp: %s %s (CSeq=%d, body=%zu)\n",
           req->method, req->uri, req->cseq, req->body_len);

    /* POST-based endpoints keyed on URI */
    if (strcmp(req->method, "POST") == 0) {
        if (strcmp(req->uri, "/pair-setup") == 0)
            return handle_pair_setup(sess, fd, req);
        if (strcmp(req->uri, "/pair-verify") == 0)
            return handle_pair_verify(sess, fd, req);
        if (strcmp(req->uri, "/fp-setup") == 0)
            return handle_fp_setup(sess, fd, req);
        if (strcmp(req->uri, "/info") == 0)
            return handle_info(sess, fd, req->cseq);
        if (strcmp(req->uri, "/feedback") == 0)
            return handle_feedback(fd, req);
        /* Unknown POST — 200 OK */
        return rtsp_ok(fd, req->cseq);
    }

    if (strcmp(req->method, "GET") == 0) {
        if (strcmp(req->uri, "/info") == 0)
            return handle_info(sess, fd, req->cseq);
        return rtsp_ok(fd, req->cseq);
    }

    if (strcmp(req->method, "OPTIONS") == 0)
        return handle_options(fd, req->cseq);

    if (strcmp(req->method, "ANNOUNCE") == 0)
        return handle_announce(sess, fd, req);

    if (strcmp(req->method, "SETUP") == 0)
        return handle_setup(sess, fd, req);

    if (strcmp(req->method, "RECORD") == 0)
        return handle_record(sess, fd, req);

    if (strcmp(req->method, "GET_PARAMETER") == 0)
        return handle_get_parameter(fd, req);

    if (strcmp(req->method, "SET_PARAMETER") == 0)
        return handle_set_parameter(sess, fd, req);

    if (strcmp(req->method, "TEARDOWN") == 0)
        return handle_teardown(sess, fd, req);

    if (strcmp(req->method, "FLUSH") == 0)
        return rtsp_ok(fd, req->cseq);

    /* Default: 200 OK for unknown methods */
    fprintf(stderr, "rtsp: unhandled method %s %s\n", req->method, req->uri);
    return rtsp_ok(fd, req->cseq);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

int rtsp_session_init(airplay_rtsp_session_t *sess,
                       const char *mac,
                       const char *device_name,
                       airplay_video_cb_t video_cb,
                       airplay_audio_cb_t audio_cb,
                       void *cb_ctx)
{
    if (!sess) return -1;
    memset(sess, 0, sizeof(*sess));
    sess->state   = RTSP_STATE_IDLE;
    sess->rtsp_fd = -1;

    strncpy(sess->device_mac,  mac         ? mac         : "AA:BB:CC:DD:EE:FF",
            sizeof(sess->device_mac)  - 1);
    strncpy(sess->device_name, device_name ? device_name : "CarPlay",
            sizeof(sess->device_name) - 1);

    sess->video_cb  = video_cb;
    sess->audio_cb  = audio_cb;
    sess->cb_ctx    = cb_ctx;
    sess->hid_cseq  = 1000;

    /* Init pairing context */
    if (pair_ctx_init(&sess->pair, mac) < 0) return -1;

    /* Init FairPlay context */
    if (fairplay_ctx_init(&sess->fairplay) < 0) return -1;

    /* Init mirror context */
    if (airplay_mirror_ctx_init(&sess->mirror,
                                  (mirror_video_cb_t)video_cb,
                                  cb_ctx) < 0) return -1;

    return 0;
}

int rtsp_session_handle(airplay_rtsp_session_t *sess, int fd)
{
    if (!sess || fd < 0) return -1;
    sess->rtsp_fd = fd;

    /* Enable TCP_NODELAY for low latency */
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    printf("rtsp: session started\n");

    for (;;) {
        rtsp_request_t req;
        if (rtsp_read_request(fd, &req) < 0) {
            printf("rtsp: client disconnected\n");
            break;
        }

        int ret = rtsp_dispatch(sess, fd, &req);
        rtsp_request_free(&req);

        if (ret < 0) {
            printf("rtsp: session ended (ret=%d)\n", ret);
            break;
        }
    }

    sess->rtsp_fd = -1;
    sess->state   = RTSP_STATE_TEARDOWN;
    return 0;
}

/*
 * Send a HID touch event back to the iPhone via SET_PARAMETER.
 *
 * AirPlay HID touch events are sent as:
 *   SET_PARAMETER rtsp://... RTSP/1.0
 *   Content-Type: application/x-apple-binary-plist
 *   [binary plist with HID event data]
 *
 * For simplicity we send a minimal text/parameters format that iOS accepts:
 *   "hid: <type> <x> <y>\r\n"
 */
int rtsp_send_touch(airplay_rtsp_session_t *sess,
                     float x, float y, int type)
{
    if (!sess || sess->rtsp_fd < 0) return -1;
    if (sess->state != RTSP_STATE_RECORDING) return -1;

    char body[128];
    int body_len = snprintf(body, sizeof(body),
                             "hid: %d %.6f %.6f\r\n", type, x, y);

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "SET_PARAMETER rtsp://localhost/session RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "Content-Type: text/parameters\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        sess->hid_cseq++, body_len, body);

    if (send(sess->rtsp_fd, req, req_len, MSG_NOSIGNAL) < 0) return -1;
    return 0;
}

void rtsp_session_destroy(airplay_rtsp_session_t *sess)
{
    if (!sess) return;
    pair_ctx_destroy(&sess->pair);
    fairplay_ctx_destroy(&sess->fairplay);
    airplay_mirror_ctx_destroy(&sess->mirror);
    if (sess->rtsp_fd >= 0) {
        close(sess->rtsp_fd);
        sess->rtsp_fd = -1;
    }
}

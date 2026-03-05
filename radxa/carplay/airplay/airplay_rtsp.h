#pragma once

/*
 * airplay_rtsp.h — RTSP Session Handler for AirPlay CarPlay
 *
 * AirPlay 2 screen mirroring uses RTSP as the control plane.
 * The iPhone connects to port 7000 and issues the following request sequence:
 *
 *   POST /pair-setup     — SRP-6a pairing key exchange (M1..M4)
 *   POST /pair-verify    — Ed25519 + X25519 session authentication
 *   POST /fp-setup       — FairPlay DRM handshake (3 stages)
 *   OPTIONS *            — capability probe
 *   POST /info           — device info exchange (binary plist)
 *   ANNOUNCE             — SDP describing the video/audio streams
 *   SETUP                — allocate ports for video mirror and audio
 *   RECORD               — begin streaming
 *   GET_PARAMETER        — periodic keep-alive / parameter query
 *   SET_PARAMETER        — volume, brightness, HID events
 *   TEARDOWN             — end session
 *
 * The body format varies by endpoint:
 *   /pair-setup  /pair-verify  : raw binary TLV8
 *   /fp-setup                  : raw binary blob
 *   /info                      : binary Apple plist
 *   ANNOUNCE                   : SDP (text/parameters)
 *   SET_PARAMETER              : text/parameters or application/x-apple-binary-plist
 *
 * Note on binary plists:
 *   Apple sends binary plists for /info. A minimal parser is included below
 *   that handles the subset needed. Full parsing requires a proper bplist library;
 *   for this implementation we return a hardcoded /info response.
 *
 * RTSP response format:
 *   RTSP/1.0 <code> <reason>\r\n
 *   CSeq: <cseq>\r\n
 *   [Content-Type: <type>\r\n]
 *   [Content-Length: <len>\r\n]
 *   \r\n
 *   [body]
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "airplay_pair.h"
#include "airplay_fairplay.h"
#include "airplay_mirror.h"

/*
 * Callback typedefs (also defined in airplay_server.h — matching signatures).
 * Defined here so airplay_rtsp.h can stand alone without pulling in server.h.
 */
typedef void (*airplay_video_cb_t)(const uint8_t *data, size_t len,
                                    uint64_t timestamp_ns, void *ctx);
typedef void (*airplay_audio_cb_t)(const uint8_t *audio_data, size_t len,
                                    int format, void *ctx);

/* Default port allocations for dynamically allocated streams */
#define AIRPLAY_AUDIO_DATA_PORT     7010
#define AIRPLAY_AUDIO_CONTROL_PORT  7011
#define AIRPLAY_MIRROR_PORT_DEFAULT 7100

/* Maximum RTSP request size we will buffer */
#define RTSP_MAX_REQUEST_BYTES  (256 * 1024)

/* RTSP session state machine */
typedef enum {
    RTSP_STATE_IDLE      = 0,
    RTSP_STATE_PAIRED    = 1,   /* pair-setup done */
    RTSP_STATE_VERIFIED  = 2,   /* pair-verify done */
    RTSP_STATE_FP_DONE   = 3,   /* fp-setup done, AES key ready */
    RTSP_STATE_ANNOUNCED = 4,   /* ANNOUNCE processed */
    RTSP_STATE_SETUP     = 5,   /* SETUP processed, ports allocated */
    RTSP_STATE_RECORDING = 6,   /* RECORD received, streaming */
    RTSP_STATE_TEARDOWN  = 7,   /* session ending */
} rtsp_state_t;

/* Stream setup info returned by SETUP response */
typedef struct {
    uint16_t audio_data_port;     /* UDP or TCP port for audio RTP data */
    uint16_t audio_control_port;  /* UDP or TCP port for audio RTCP */
    uint16_t video_port;          /* TCP port for mirror stream */
    bool     video_tcp;           /* true = TCP mirror stream */
    bool     audio_active;
    bool     video_active;
} rtsp_stream_setup_t;

/*
 * Per-connection RTSP context.
 * The server allocates one of these per accepted connection.
 */
typedef struct {
    rtsp_state_t state;

    /* Pairing and FairPlay contexts (owned by this connection) */
    airplay_pair_ctx_t     pair;
    airplay_fairplay_ctx_t fairplay;

    /* Mirror context (owned by this connection) */
    airplay_mirror_ctx_t   mirror;

    /* Stream configuration negotiated during SETUP */
    rtsp_stream_setup_t    streams;

    /* Device MAC address (for /info response) */
    char device_mac[18];   /* "AA:BB:CC:DD:EE:FF\0" */
    char device_name[64];

    /* Callbacks forwarded from the main server */
    airplay_video_cb_t video_cb;
    airplay_audio_cb_t audio_cb;
    void              *cb_ctx;

    /* Touch event socket back to iPhone (the RTSP control connection fd) */
    int rtsp_fd;

    /* Per-session HID CSeq counter for SET_PARAMETER touch events */
    int hid_cseq;
} airplay_rtsp_session_t;

/*
 * Initialise an RTSP session context.
 * mac should be "AA:BB:CC:DD:EE:FF" format.
 */
int rtsp_session_init(airplay_rtsp_session_t *sess,
                       const char *mac,
                       const char *device_name,
                       airplay_video_cb_t video_cb,
                       airplay_audio_cb_t audio_cb,
                       void *cb_ctx);

/*
 * Handle one RTSP connection from start to TEARDOWN.
 * fd is the accepted TCP socket.
 * Blocks until the session ends.
 */
int rtsp_session_handle(airplay_rtsp_session_t *sess, int fd);

/*
 * Send a HID touch event to the connected iPhone.
 * Must be called while session is in RECORDING state.
 * x, y are 0.0–1.0 normalised.  type: 0=down, 1=move, 2=up.
 */
int rtsp_send_touch(airplay_rtsp_session_t *sess,
                     float x, float y, int type);

void rtsp_session_destroy(airplay_rtsp_session_t *sess);

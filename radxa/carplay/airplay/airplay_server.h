#pragma once

/*
 * airplay_server.h — AirPlay Receiver Top-Level Interface
 *
 * After iAP2 authentication succeeds on the T-Dongle-S3 → Radxa link,
 * the iPhone connects to this AirPlay receiver over WiFi.
 *
 * Protocol flow:
 *   1. mDNS advertisement (_airplay._tcp, port 7000) — via Avahi
 *   2. iPhone discovers device, initiates RTSP connection to port 7000
 *   3. POST /pair-setup    — SRP-6a key exchange
 *   4. POST /pair-verify   — Ed25519 + X25519 session authentication
 *   5. POST /fp-setup      — FairPlay DRM handshake (3 stages)
 *   6. ANNOUNCE            — SDP negotiation (video + audio)
 *   7. SETUP               — allocate video mirror port + audio RTP port
 *   8. RECORD              — begin streaming
 *   9. H.264 video stream  — on AIRPLAY_MIRROR_PORT (TCP)
 *  10. Audio RTP stream    — on audio data port (UDP)
 *  11. SET_PARAMETER       — volume, HID touch events
 *  12. TEARDOWN            — end session
 *
 * Threading model:
 *   - One "listener" thread accepts incoming RTSP connections (port 7000)
 *   - Each accepted RTSP connection spawns an rtsp_handler_thread
 *   - Mirror data connections (port 7100) are accepted by a separate thread
 *   - Audio is handled inside airplay_audio_ctx (2 threads: recv + play)
 *   - mDNS runs in its own thread (Avahi internal)
 */

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "airplay_mdns.h"
#include "airplay_rtsp.h"
#include "airplay_audio.h"

#define AIRPLAY_PORT         7000    /* RTSP control port */
#define AIRPLAY_MIRROR_PORT  7100    /* TCP mirror stream port */

/* Maximum simultaneous RTSP sessions (typically just 1 for CarPlay) */
#define AIRPLAY_MAX_SESSIONS 2

/*
 * Callback types are defined in airplay_rtsp.h (included above via the
 * include chain: airplay_server.h → airplay_rtsp.h).
 * airplay_video_cb_t and airplay_audio_cb_t are available after the include.
 */

/*
 * Per-session state, kept by the server for each active connection.
 */
typedef struct {
    airplay_rtsp_session_t rtsp;   /* RTSP session context */
    airplay_audio_ctx_t    audio;  /* Audio receive + playback context */
    pthread_t              rtsp_thread;
    pthread_t              mirror_thread;
    bool                   active;
    int                    rtsp_client_fd;
    int                    mirror_client_fd;
} airplay_session_t;

/*
 * Top-level server state.
 */
typedef struct {
    /* Listener file descriptors */
    int rtsp_listen_fd;    /* TCP listener on AIRPLAY_PORT */
    int mirror_listen_fd;  /* TCP listener on AIRPLAY_MIRROR_PORT */

    /* mDNS advertisement context */
    airplay_mdns_ctx_t *mdns;

    /* Device identity */
    char device_mac[18];   /* "AA:BB:CC:DD:EE:FF\0" */
    char device_name[64];  /* Shown in iOS AirPlay picker */

    /* Callbacks */
    airplay_video_cb_t video_cb;
    airplay_audio_cb_t audio_cb;
    void              *cb_ctx;

    /* Active sessions */
    airplay_session_t sessions[AIRPLAY_MAX_SESSIONS];
    pthread_mutex_t   sessions_lock;

    /* Listener threads */
    pthread_t rtsp_accept_thread;
    pthread_t mirror_accept_thread;

    /* Serialize mirror accept() across sessions */
    pthread_mutex_t mirror_accept_lock;

    volatile int running;

    /* Legacy compatibility fields (from original skeleton) */
    bool connected;
    bool mirroring;
} airplay_server_t;

/*
 * Initialise the AirPlay server.
 *
 * mac         — device MAC address "AA:BB:CC:DD:EE:FF" (must be unique on LAN)
 * device_name — name shown in iOS AirPlay picker
 * video_cb    — called for each H.264/H.265 frame (may be NULL)
 * audio_cb    — called for decoded audio (may be NULL; use ALSA instead)
 * ctx         — opaque context passed to callbacks
 *
 * On success: starts mDNS advertisement and RTSP/mirror listeners.
 * Returns 0 on success, -1 on error.
 */
int airplay_server_init(airplay_server_t *srv,
                         const char *mac,
                         const char *device_name,
                         airplay_video_cb_t video_cb,
                         airplay_audio_cb_t audio_cb,
                         void *ctx);

/*
 * Start the server accept loops (launches background threads).
 * Returns immediately. Call airplay_server_wait() to block until stopped.
 */
int airplay_server_start(airplay_server_t *srv);

/*
 * Block until the server is stopped (via airplay_server_stop()).
 */
void airplay_server_wait(airplay_server_t *srv);

/*
 * Run the server synchronously (blocks until stopped).
 * Equivalent to airplay_server_start() + airplay_server_wait().
 */
int airplay_server_run(airplay_server_t *srv);

/*
 * Send a touch event to the connected iPhone.
 * x, y are normalised (0.0 – 1.0).
 * type: 0 = touch down, 1 = touch move, 2 = touch up.
 */
int airplay_send_touch(airplay_server_t *srv,
                        float x, float y, int touch_type);

/*
 * Stop the server gracefully.
 * Disconnects all sessions, unregisters mDNS, closes listeners.
 */
void airplay_server_stop(airplay_server_t *srv);

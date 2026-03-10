/*
 * CarPlay Daemon — Entry Point
 *
 * Ties together MFi auth, Bluetooth iAP2 transport, iAP2 link/session/auth,
 * CarPlay EAP session, and the AirPlay video receiver.  Video frames from
 * AirPlay are forwarded to the compositor via a Unix domain socket at
 * /tmp/carplay-video.sock using a 4-byte big-endian length prefix followed by
 * the raw H.264 Annex-B data.
 *
 * Startup sequence:
 *   1. Open MFi I2C chip — fatal if chip absent (CarPlay requires MFi auth).
 *   2. Init Bluetooth transport (BlueZ RFCOMM listener + SDP records).
 *   3. Start mDNS advertisement for AirPlay (_airplay._tcp on port 7000).
 *   4. Start AirPlay RTSP server in background thread.
 *   5. Connect to compositor Unix socket (retry loop, compositor may start
 *      after us).
 *   6. Accept iPhone BT connection (blocking).
 *   7. Run iAP2 link layer SYN/ACK handshake.
 *   8. Run iAP2 session: identification → MFi cert → challenge → auth.
 *   9. Start CarPlay EAP session.
 *  10. Block until iPhone disconnects, then go back to step 6.
 *
 * Signal handling:
 *   SIGINT / SIGTERM — clean shutdown: stop AirPlay, cleanup BT, close MFi.
 *
 * Build:
 *   gcc -o carplay_daemon main.c mfi/mfi_auth.c iap2/ airplay/ \
 *       -lbluetooth -lpthread -lm  (see Makefile for full source list)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include "mfi/mfi_auth.h"
#include "iap2/iap2_bt_transport.h"
#include "iap2/iap2_link.h"
#include "iap2/iap2_session.h"
#include "iap2/iap2_carplay.h"
#include "airplay/airplay_server.h"

/* -------------------------------------------------------------------------
 * Compile-time constants
 * ---------------------------------------------------------------------- */

#define CARPLAY_VIDEO_SOCK   "/tmp/carplay-video.sock"
#define COMPOSITOR_CONNECT_RETRIES  30
#define COMPOSITOR_CONNECT_DELAY_MS 1000

/* Log levels */
#define LOG_INFO  0
#define LOG_WARN  1
#define LOG_ERROR 2

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;

/*
 * g_mfi_dev — MFi coprocessor handle.
 * Non-static so iap2_session.c can reference it via:
 *   extern mfi_device_t g_mfi_dev;
 */
mfi_device_t             g_mfi_dev;
static iap2_bt_t         g_bt;
static airplay_server_t  g_airplay;
static iap2_carplay_t    g_carplay;
static int               g_compositor_fd = -1;
static pthread_mutex_t   g_compositor_mutex = PTHREAD_MUTEX_INITIALIZER;

/* AirPlay server thread */
static pthread_t         g_airplay_thread;

/* -------------------------------------------------------------------------
 * Logging
 * ---------------------------------------------------------------------- */

static void log_msg(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void log_msg(int level, const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);

    const char *prefix;
    FILE *out;
    switch (level) {
    case LOG_WARN:  prefix = "WARN ";  out = stderr; break;
    case LOG_ERROR: prefix = "ERROR";  out = stderr; break;
    default:        prefix = "INFO ";  out = stdout; break;
    }

    fprintf(out, "[%s.%03ld] [%s] ", timebuf,
            (long)(ts.tv_nsec / 1000000), prefix);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fprintf(out, "\n");
    fflush(out);
}

#define LOG_I(fmt, ...)  log_msg(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...)  log_msg(LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...)  log_msg(LOG_ERROR, fmt, ##__VA_ARGS__)

/* -------------------------------------------------------------------------
 * Signal handling
 * ---------------------------------------------------------------------- */

static void signal_handler(int signum)
{
    (void)signum;
    g_running = 0;
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE — we handle write errors manually */
    signal(SIGPIPE, SIG_IGN);
}

/* -------------------------------------------------------------------------
 * Compositor Unix socket
 * ---------------------------------------------------------------------- */

/*
 * Open a connection to the compositor's video input socket.
 * Retries up to COMPOSITOR_CONNECT_RETRIES times with a 1 s delay.
 * Returns socket fd on success, -1 on permanent failure.
 */
static int compositor_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_E("compositor_connect: socket(): %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CARPLAY_VIDEO_SOCK, sizeof(addr.sun_path) - 1);

    for (int attempt = 0; attempt < COMPOSITOR_CONNECT_RETRIES; attempt++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            LOG_I("Connected to compositor at %s", CARPLAY_VIDEO_SOCK);
            return fd;
        }
        if (errno != ENOENT && errno != ECONNREFUSED) {
            LOG_E("compositor_connect: connect(): %s", strerror(errno));
            close(fd);
            return -1;
        }
        LOG_I("Compositor not ready, retrying (%d/%d)...",
              attempt + 1, COMPOSITOR_CONNECT_RETRIES);
        usleep(COMPOSITOR_CONNECT_DELAY_MS * 1000);
    }

    LOG_E("compositor_connect: timed out waiting for %s", CARPLAY_VIDEO_SOCK);
    close(fd);
    return -1;
}

/*
 * Send a video frame to the compositor.
 * Frame format: [4-byte BE length][H.264 Annex-B data]
 */
static void compositor_send_video(const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&g_compositor_mutex);

    if (g_compositor_fd < 0) {
        pthread_mutex_unlock(&g_compositor_mutex);
        return;
    }

    uint8_t hdr[4];
    hdr[0] = (len >> 24) & 0xFF;
    hdr[1] = (len >> 16) & 0xFF;
    hdr[2] = (len >>  8) & 0xFF;
    hdr[3] = (len      ) & 0xFF;

    /* Write header */
    ssize_t n = write(g_compositor_fd, hdr, sizeof(hdr));
    if (n != (ssize_t)sizeof(hdr)) {
        LOG_W("compositor_send_video: header write failed, will reconnect");
        close(g_compositor_fd);
        g_compositor_fd = -1;
        pthread_mutex_unlock(&g_compositor_mutex);
        /* Reconnect outside the mutex to avoid blocking other threads */
        int new_fd = compositor_connect();
        pthread_mutex_lock(&g_compositor_mutex);
        if (g_compositor_fd < 0) g_compositor_fd = new_fd;
        else close(new_fd); /* another thread reconnected first */
        pthread_mutex_unlock(&g_compositor_mutex);
        return;
    }

    /* Write payload */
    size_t written = 0;
    while (written < len) {
        n = write(g_compositor_fd, data + written, len - written);
        if (n <= 0) {
            LOG_W("compositor_send_video: payload write failed, will reconnect");
            close(g_compositor_fd);
            g_compositor_fd = -1;
            pthread_mutex_unlock(&g_compositor_mutex);
            int new_fd = compositor_connect();
            pthread_mutex_lock(&g_compositor_mutex);
            if (g_compositor_fd < 0) g_compositor_fd = new_fd;
            else close(new_fd);
            pthread_mutex_unlock(&g_compositor_mutex);
            return;
        }
        written += (size_t)n;
    }

    pthread_mutex_unlock(&g_compositor_mutex);
}

/* -------------------------------------------------------------------------
 * AirPlay callbacks
 * ---------------------------------------------------------------------- */

static void on_airplay_video(const uint8_t *h264_data, size_t len,
                               uint64_t timestamp_ns, void *ctx)
{
    (void)timestamp_ns;
    (void)ctx;
    compositor_send_video(h264_data, len);
}

static void on_airplay_audio(const uint8_t *audio_data, size_t len,
                               int format, void *ctx)
{
    /*
     * Audio forwarding to the compositor or audio output daemon can be
     * added here.  For now, audio is silently discarded — the car's own
     * audio stack handles output when AA video is in the foreground.
     */
    (void)audio_data;
    (void)len;
    (void)format;
    (void)ctx;
}

/* -------------------------------------------------------------------------
 * AirPlay server thread
 * ---------------------------------------------------------------------- */

static void *airplay_thread_fn(void *arg)
{
    (void)arg;
    LOG_I("AirPlay server starting on port %d", AIRPLAY_PORT);
    int rc = airplay_server_run(&g_airplay);
    if (rc != 0 && g_running) {
        LOG_E("AirPlay server exited with error %d", rc);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * CarPlay session callbacks
 * ---------------------------------------------------------------------- */

/*
 * Called by iap2_carplay when the wireless CarPlay session is fully up
 * (StartWirelessCarPlaySession received, WiFi credentials exchanged).
 * At this point the iPhone will connect to our AirPlay server over WiFi.
 */
static void on_carplay_ready(const cp_wifi_info_t *wifi, void *ctx)
{
    (void)ctx;
    LOG_I("CarPlay wireless session ready — iPhone will connect via AirPlay");
    LOG_I("  WiFi SSID: %s", wifi->ssid);
    LOG_I("  AirPlay port: %d", wifi->airplay_port);

    /*
     * The AirPlay server is already running (started in main before the BT
     * accept loop).  Nothing further to do here — the iPhone will initiate
     * the RTSP connection on its own once it joins the WiFi AP.
     *
     * Signal the carplay object that AirPlay is ready on our side so it can
     * update its internal state.
     */
    iap2_carplay_on_airplay_ready(&g_carplay);
}

/*
 * Called by iap2_carplay when the iPhone ends the CarPlay session.
 */
static void on_carplay_stopped(void *ctx)
{
    (void)ctx;
    LOG_I("CarPlay session stopped by iPhone");
}

/* -------------------------------------------------------------------------
 * iAP2 link rx callback
 *
 * Dispatches incoming link-layer packets to the session layer (control
 * channel) or the CarPlay EAP handler (EAP channel).
 * ---------------------------------------------------------------------- */

typedef struct {
    iap2_session_t  *sess;
    iap2_carplay_t  *carplay;
    bool             carplay_started; /* have we called iap2_carplay_start? */
} link_rx_ctx_t;

static void on_iap2_link_rx(const iap2_packet_t *pkt, void *ctx)
{
    link_rx_ctx_t *lctx = (link_rx_ctx_t *)ctx;

    if (pkt->payload_len < 4) {
        LOG_W("iAP2 rx: packet too short (%zu bytes)", pkt->payload_len);
        return;
    }

    /* iAP2 session message header: 2-byte length + 2-byte message ID */
    uint16_t msg_id = ((uint16_t)pkt->payload[2] << 8) | pkt->payload[3];
    const uint8_t *msg_payload = pkt->payload + 4;
    size_t msg_payload_len = pkt->payload_len - 4;

    if (pkt->session_id == IAP2_SESSION_EAP) {
        /*
         * EAP channel packet — route to the CarPlay handler.
         * Pass msg_id = 0x0000 as the sentinel for raw EAP payloads;
         * the actual CarPlay sub-message ID is in the first 2 bytes of
         * msg_payload (handled inside iap2_carplay_handle_message).
         */
        if (lctx->carplay_started) {
            /* Pass the raw EAP payload (skip the 4-byte session header) */
            iap2_carplay_handle_message(lctx->carplay, 0x0000,
                                        msg_payload, msg_payload_len);
        } else {
            LOG_W("iAP2 rx: EAP packet received before CarPlay started, ignoring");
        }
    } else {
        /*
         * Control channel (or any non-EAP session) — let the session layer
         * handle it.  It will set sess->carplay_started when it receives
         * IAP2_MSG_START_EAP_SESSION; we check that flag below.
         *
         * Also route CarPlay EAP control messages (EAPSessionStarted,
         * StopEAPSession) to the carplay handler so it can track EAP state.
         */
        iap2_session_handle_message(lctx->sess, msg_id,
                                    msg_payload, msg_payload_len);

        /* Let the CarPlay handler see control-channel messages too
         * (it needs IAP2_MSG_EAP_SESSION_STARTED / IAP2_MSG_STOP_EAP_SESSION). */
        if (lctx->carplay_started) {
            iap2_carplay_handle_message(lctx->carplay, msg_id,
                                        msg_payload, msg_payload_len);
        }
    }
}

/* -------------------------------------------------------------------------
 * Single connection handler
 *
 * Called with the RFCOMM fd for one iPhone connection.
 * Runs the full iAP2 link + session + CarPlay state machine.
 * Returns when the connection drops or g_running is cleared.
 * ---------------------------------------------------------------------- */

static void handle_iphone_connection(int rfcomm_fd)
{
    LOG_I("iPhone connected, starting iAP2 link layer...");

    iap2_session_t sess;
    iap2_link_t    link;
    link_rx_ctx_t  rx_ctx = {
        .sess            = &sess,
        .carplay         = &g_carplay,
        .carplay_started = false,
    };

    /* Initialize link layer */
    if (iap2_link_init(&link, rfcomm_fd, on_iap2_link_rx, &rx_ctx) != 0) {
        LOG_E("iap2_link_init failed");
        goto cleanup;
    }

    /* Initialize session layer */
    if (iap2_session_init(&sess, &link) != 0) {
        LOG_E("iap2_session_init failed");
        goto cleanup;
    }

    /*
     * iap2_session_init already pre-loads the MFi certificate from g_mfi_dev
     * (see iap2_session.c).  The redundant mfi_get_certificate call that was
     * here previously has been removed to avoid a double-read.
     */
    LOG_I("iAP2 session initialized (MFi cert: %d bytes)",
          sess.mfi_cert_len);

    /* Send SYN to initiate iAP2 link negotiation */
    if (iap2_link_send_syn(&link) != 0) {
        LOG_E("iap2_link_send_syn failed");
        goto cleanup;
    }

    /* Send device identification */
    if (iap2_session_send_identification(&sess) != 0) {
        LOG_E("iap2_session_send_identification failed");
        goto cleanup;
    }

    /*
     * Reset the CarPlay handler for this connection so it is ready to
     * start fresh (iap2_carplay_init was called once in main; subsequent
     * connections reuse g_carplay after reinitialisation here).
     */
    iap2_carplay_init(&g_carplay,
                      on_carplay_ready,  NULL,
                      on_carplay_stopped, NULL);

    /*
     * Process link layer packets until:
     *   - The connection drops (iap2_link_process returns < 0)
     *   - We are signalled to shut down (g_running cleared)
     *
     * When the session layer sees the iPhone has accepted identification
     * and completed MFi auth, it sets sess.carplay_started after receiving
     * IAP2_MSG_START_EAP_SESSION.  We detect that transition here and
     * launch the CarPlay EAP session exactly once per connection.
     */
    while (g_running) {
        int rc = iap2_link_process(&link);
        if (rc < 0) {
            LOG_I("iAP2 link disconnected (rc=%d)", rc);
            break;
        }

        /*
         * Check if the session layer just signalled that the iPhone wants
         * to start the CarPlay EAP session.  Call iap2_carplay_start() once.
         */
        if (sess.carplay_started && !rx_ctx.carplay_started) {
            LOG_I("iAP2 auth complete — starting CarPlay EAP session");
            rx_ctx.carplay_started = true;

            if (iap2_carplay_start(&g_carplay, &sess) != 0) {
                LOG_E("iap2_carplay_start failed — dropping connection");
                break;
            }
            LOG_I("CarPlay EAP session started, waiting for iPhone WiFi join");
        }
    }

cleanup:
    /* Tear down the CarPlay session cleanly if it is still active */
    if (rx_ctx.carplay_started) {
        iap2_carplay_stop(&g_carplay);
    }

    LOG_I("iPhone connection closed");
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    LOG_I("=== CarPlay Daemon v1.0 starting ===");

    setup_signals();

    /* --- Step 1: Initialize MFi chip --- */
    LOG_I("Opening MFi auth chip on %s addr=0x%02X",
          MFI_I2C_BUS, MFI_I2C_ADDR);

    if (mfi_open(&g_mfi_dev, MFI_I2C_BUS, MFI_I2C_ADDR) != 0) {
        LOG_E("Failed to open MFi chip — is it wired to %s?", MFI_I2C_BUS);
        return EXIT_FAILURE;
    }

    int mfi_ver = mfi_get_version(&g_mfi_dev);
    if (mfi_ver < 0) {
        LOG_E("MFi chip not responding — check I2C wiring");
        mfi_close(&g_mfi_dev);
        return EXIT_FAILURE;
    }
    LOG_I("MFi chip version: 0x%02X", (unsigned)mfi_ver);

    /* --- Step 2: Initialize Bluetooth transport --- */
    LOG_I("Initializing Bluetooth RFCOMM transport...");

    if (iap2_bt_init(&g_bt, NULL) != 0) {
        LOG_E("iap2_bt_init failed — is bluetoothd running?");
        mfi_close(&g_mfi_dev);
        return EXIT_FAILURE;
    }
    LOG_I("BT adapter: %s, RFCOMM channel: %d",
          g_bt.adapter, g_bt.rfcomm_channel);

    /* --- Step 3 & 4: Start AirPlay server (includes mDNS advertisement) --- */
    LOG_I("Starting AirPlay receiver (port %d)...", AIRPLAY_PORT);

    if (airplay_server_init(&g_airplay, NULL, NULL, on_airplay_video, on_airplay_audio, NULL) != 0) {
        LOG_E("airplay_server_init failed");
        iap2_bt_cleanup(&g_bt);
        mfi_close(&g_mfi_dev);
        return EXIT_FAILURE;
    }

    if (pthread_create(&g_airplay_thread, NULL, airplay_thread_fn, NULL) != 0) {
        LOG_E("Failed to start AirPlay thread: %s", strerror(errno));
        iap2_bt_cleanup(&g_bt);
        mfi_close(&g_mfi_dev);
        return EXIT_FAILURE;
    }
    LOG_I("AirPlay thread started");

    /* --- Step 4b: Pre-initialize CarPlay handler (callbacks set per connection) --- */
    iap2_carplay_init(&g_carplay,
                      on_carplay_ready,   NULL,
                      on_carplay_stopped, NULL);

    /* --- Step 5: Connect to compositor --- */
    LOG_I("Connecting to compositor at %s...", CARPLAY_VIDEO_SOCK);
    g_compositor_fd = compositor_connect();
    if (g_compositor_fd < 0) {
        LOG_W("Compositor not available — video will be dropped until it starts");
        /* Non-fatal: compositor_send_video will retry on each frame */
    }

    /* --- Main loop: accept iPhone connections --- */
    LOG_I("Waiting for iPhone BT connection...");

    while (g_running) {
        int rfcomm_fd = iap2_bt_accept(&g_bt);

        if (rfcomm_fd < 0) {
            if (!g_running) {
                break;  /* Shutdown in progress */
            }
            LOG_W("iap2_bt_accept failed: %s — retrying in 3s", strerror(errno));
            sleep(3);
            continue;
        }

        LOG_I("Accepted RFCOMM connection (fd=%d)", rfcomm_fd);

        /* Run the full iAP2 + CarPlay state machine for this connection */
        handle_iphone_connection(rfcomm_fd);

        iap2_bt_close(rfcomm_fd);
        LOG_I("Connection closed. Waiting for next iPhone connection...");
    }

    /* --- Clean shutdown --- */
    LOG_I("Shutting down...");

    airplay_server_stop(&g_airplay);
    pthread_join(g_airplay_thread, NULL);
    LOG_I("AirPlay server stopped");

    iap2_bt_cleanup(&g_bt);
    LOG_I("Bluetooth transport released");

    if (g_compositor_fd >= 0) {
        close(g_compositor_fd);
    }

    mfi_close(&g_mfi_dev);
    LOG_I("MFi chip closed");

    LOG_I("CarPlay daemon exited cleanly");
    return EXIT_SUCCESS;
}

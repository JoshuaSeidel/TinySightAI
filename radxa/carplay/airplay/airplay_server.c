/*
 * airplay_server.c — AirPlay Receiver Top-Level Server
 *
 * Orchestrates all AirPlay components:
 *   - mDNS advertisement (Avahi)
 *   - RTSP control plane (pair-setup, pair-verify, fp-setup, ANNOUNCE, SETUP, RECORD)
 *   - TCP mirror stream (H.264/H.265 video)
 *   - UDP audio stream (AAC/ALAC/PCM via RTP)
 *
 * Threading model:
 *   main caller          → calls airplay_server_init() + airplay_server_run()
 *   rtsp_accept_thread   → accept() loop on port 7000
 *     rtsp_handler_thread → handles one RTSP session (ANNOUNCE/SETUP/RECORD/etc.)
 *   mirror_accept_thread → accept() loop on port 7100
 *     mirror_handler_thread → receives H.264 frames from mirror connection
 *   mdns internal thread → Avahi event loop
 *   audio recv_thread    → UDP receive + libavcodec decode
 *   audio play_thread    → ALSA write
 */

#include "airplay_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include "airplay_rtsp.h"
#include "airplay_mirror.h"
#include "airplay_audio.h"
#include "airplay_mdns.h"

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static int create_tcp_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * Read the MAC address from the first non-loopback interface.
 * Falls back to the provided default if ioctl fails.
 */
static void read_system_mac(char *out_mac, size_t out_len)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        snprintf(out_mac, out_len, "%s", "AA:BB:CC:DD:EE:FF");
        return;
    }

    struct ifreq ifr;
    /* Try common interface names */
    const char *ifaces[] = { "ap0", "wlan0", "eth0", "end0", NULL };
    bool found = false;

    for (int i = 0; ifaces[i] && !found; i++) {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifaces[i], IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
            unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
            snprintf(out_mac, out_len,
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            found = true;
        }
    }
    close(sock);

    if (!found) {
        snprintf(out_mac, out_len, "%s", "AA:BB:CC:DD:EE:FF");
    }
}

/* -----------------------------------------------------------------------
 * Session management
 * ----------------------------------------------------------------------- */

static airplay_session_t *find_free_session(airplay_server_t *srv)
{
    for (int i = 0; i < AIRPLAY_MAX_SESSIONS; i++) {
        if (!srv->sessions[i].active) return &srv->sessions[i];
    }
    return NULL;
}

static void session_cleanup(airplay_session_t *sess)
{
    rtsp_session_destroy(&sess->rtsp);
    airplay_audio_stop(&sess->audio);
    airplay_audio_ctx_destroy(&sess->audio);

    if (sess->rtsp_client_fd >= 0) {
        close(sess->rtsp_client_fd);
        sess->rtsp_client_fd = -1;
    }
    if (sess->mirror_client_fd >= 0) {
        close(sess->mirror_client_fd);
        sess->mirror_client_fd = -1;
    }
    sess->active = false;
}

/* -----------------------------------------------------------------------
 * Thread: handle one RTSP session + its mirror stream
 * ----------------------------------------------------------------------- */

typedef struct {
    airplay_server_t *srv;
    airplay_session_t *sess;
} session_thread_arg_t;

/*
 * Mirror thread: called after RTSP SETUP/RECORD.
 * Accepts the mirror TCP connection on AIRPLAY_MIRROR_PORT and processes it.
 */
static void *mirror_handler_thread(void *arg)
{
    session_thread_arg_t *ta = (session_thread_arg_t *)arg;
    airplay_server_t  *srv  = ta->srv;
    airplay_session_t *sess = ta->sess;
    free(ta);

    printf("server: mirror handler thread started, waiting for connection\n");

    /* Serialize mirror accept across sessions to prevent races */
    pthread_mutex_lock(&srv->mirror_accept_lock);
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(srv->mirror_listen_fd,
                            (struct sockaddr *)&client_addr, &addr_len);
    pthread_mutex_unlock(&srv->mirror_accept_lock);
    if (client_fd < 0) {
        perror("mirror accept");
        return NULL;
    }

    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sess->mirror_client_fd = client_fd;
    srv->mirroring = true;
    printf("server: mirror stream connected from %s\n",
           inet_ntoa(client_addr.sin_addr));

    airplay_mirror_handle_connection(&sess->rtsp.mirror, client_fd);

    close(client_fd);
    sess->mirror_client_fd = -1;
    srv->mirroring = false;
    printf("server: mirror stream disconnected\n");
    return NULL;
}

/*
 * RTSP handler thread: handles one iPhone RTSP connection from start to finish.
 * After SETUP, also launches the mirror handler and audio receive.
 */
static void *rtsp_handler_thread(void *arg)
{
    session_thread_arg_t *ta = (session_thread_arg_t *)arg;
    airplay_server_t  *srv  = ta->srv;
    airplay_session_t *sess = ta->sess;
    free(ta);

    int fd = sess->rtsp_client_fd;
    printf("server: RTSP handler thread started\n");

    /* Initialise RTSP session */
    if (rtsp_session_init(&sess->rtsp,
                           srv->device_mac,
                           srv->device_name,
                           srv->video_cb,
                           srv->audio_cb,
                           srv->cb_ctx) < 0) {
        fprintf(stderr, "server: rtsp_session_init failed\n");
        goto done;
    }

    srv->connected = true;

    /*
     * Launch mirror handler thread now — it will block in accept()
     * waiting for the iPhone to connect after RECORD is issued.
     */
    session_thread_arg_t *mirror_ta = malloc(sizeof(*mirror_ta));
    if (!mirror_ta) goto done;
    mirror_ta->srv  = srv;
    mirror_ta->sess = sess;
    pthread_create(&sess->mirror_thread, NULL, mirror_handler_thread, mirror_ta);

    /*
     * Init audio BEFORE entering the RTSP loop so it is ready when RECORD
     * arrives and the iPhone begins sending RTP packets.
     */
    if (sess->rtsp.streams.audio_active) {
        airplay_audio_ctx_init(&sess->audio,
                                sess->rtsp.streams.audio_data_port,
                                sess->rtsp.streams.audio_control_port,
                                AUDIO_FMT_AAC_LC);
        airplay_audio_start(&sess->audio);
    }

    /* Handle the RTSP session (blocks until TEARDOWN or disconnect) */
    rtsp_session_handle(&sess->rtsp, fd);

    /* Wait for mirror thread */
    pthread_join(sess->mirror_thread, NULL);

done:
    printf("server: RTSP handler thread exiting\n");
    srv->connected = false;

    pthread_mutex_lock(&srv->sessions_lock);
    session_cleanup(sess);
    pthread_mutex_unlock(&srv->sessions_lock);

    return NULL;
}

/* -----------------------------------------------------------------------
 * Thread: RTSP accept loop
 * ----------------------------------------------------------------------- */

static void *rtsp_accept_thread(void *arg)
{
    airplay_server_t *srv = (airplay_server_t *)arg;
    printf("server: RTSP accept loop started on port %d\n", AIRPLAY_PORT);

    while (srv->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(srv->rtsp_listen_fd,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || !srv->running) break;
            perror("RTSP accept");
            continue;
        }

        printf("server: iPhone connected from %s:%u\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        /* Find a free session slot */
        pthread_mutex_lock(&srv->sessions_lock);
        airplay_session_t *sess = find_free_session(srv);
        if (!sess) {
            pthread_mutex_unlock(&srv->sessions_lock);
            fprintf(stderr, "server: no free session slot, rejecting connection\n");
            close(client_fd);
            continue;
        }
        memset(sess, 0, sizeof(*sess));
        sess->active          = true;
        sess->rtsp_client_fd  = client_fd;
        sess->mirror_client_fd = -1;
        pthread_mutex_unlock(&srv->sessions_lock);

        /* Spawn RTSP handler thread (joinable so server_stop can wait) */
        session_thread_arg_t *ta = malloc(sizeof(*ta));
        if (!ta) { close(client_fd); sess->active = false; continue; }
        ta->srv  = srv;
        ta->sess = sess;

        pthread_create(&sess->rtsp_thread, NULL, rtsp_handler_thread, ta);
    }

    printf("server: RTSP accept loop stopped\n");
    return NULL;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

int airplay_server_init(airplay_server_t *srv,
                         const char *mac,
                         const char *device_name,
                         airplay_video_cb_t video_cb,
                         airplay_audio_cb_t audio_cb,
                         void *ctx)
{
    if (!srv) return -1;
    memset(srv, 0, sizeof(*srv));

    srv->video_cb = video_cb;
    srv->audio_cb = audio_cb;
    srv->cb_ctx   = ctx;

    /* Resolve MAC address */
    if (mac && *mac) {
        strncpy(srv->device_mac, mac, sizeof(srv->device_mac) - 1);
    } else {
        read_system_mac(srv->device_mac, sizeof(srv->device_mac));
    }

    strncpy(srv->device_name,
            (device_name && *device_name) ? device_name : "CarPlay Receiver",
            sizeof(srv->device_name) - 1);

    pthread_mutex_init(&srv->sessions_lock, NULL);
    pthread_mutex_init(&srv->mirror_accept_lock, NULL);

    /* Create RTSP listener */
    srv->rtsp_listen_fd = create_tcp_listener(AIRPLAY_PORT);
    if (srv->rtsp_listen_fd < 0) {
        fprintf(stderr, "server: failed to bind RTSP port %d\n", AIRPLAY_PORT);
        return -1;
    }

    /* Create mirror listener */
    srv->mirror_listen_fd = create_tcp_listener(AIRPLAY_MIRROR_PORT);
    if (srv->mirror_listen_fd < 0) {
        fprintf(stderr, "server: failed to bind mirror port %d\n",
                AIRPLAY_MIRROR_PORT);
        close(srv->rtsp_listen_fd);
        srv->rtsp_listen_fd = -1;
        return -1;
    }

    printf("server: AirPlay receiver '%s' (MAC=%s)\n",
           srv->device_name, srv->device_mac);
    printf("server: RTSP listening on port %d, mirror on port %d\n",
           AIRPLAY_PORT, AIRPLAY_MIRROR_PORT);

    return 0;
}

int airplay_server_start(airplay_server_t *srv)
{
    if (!srv) return -1;
    srv->running = 1;

    /* Register mDNS service — requires Ed25519 key from a session.
     * Use a temporary pairing context to get the server's public key. */
    airplay_pair_ctx_t tmp_pair;
    uint8_t ed25519_pub[32] = {0};
    if (pair_ctx_init(&tmp_pair, srv->device_mac) == 0) {
        pair_ctx_get_ed25519_pub(&tmp_pair, ed25519_pub);
        pair_ctx_destroy(&tmp_pair);
    }

    srv->mdns = airplay_mdns_register(srv->device_mac,
                                       srv->device_name,
                                       ed25519_pub,
                                       AIRPLAY_PORT);
    if (!srv->mdns) {
        fprintf(stderr, "server: mDNS registration failed (is avahi-daemon running?)\n");
        /* Non-fatal — can still work with manual IP */
    }

    /* Start RTSP accept thread */
    if (pthread_create(&srv->rtsp_accept_thread, NULL,
                        rtsp_accept_thread, srv) != 0) {
        perror("server: pthread_create rtsp_accept");
        airplay_server_stop(srv);
        return -1;
    }

    printf("server: AirPlay receiver started\n");
    return 0;
}

void airplay_server_wait(airplay_server_t *srv)
{
    if (!srv) return;
    pthread_join(srv->rtsp_accept_thread, NULL);
}

int airplay_server_run(airplay_server_t *srv)
{
    if (airplay_server_start(srv) < 0) return -1;
    airplay_server_wait(srv);
    return 0;
}

int airplay_send_touch(airplay_server_t *srv,
                        float x, float y, int touch_type)
{
    if (!srv) return -1;

    pthread_mutex_lock(&srv->sessions_lock);
    int ret = -1;
    for (int i = 0; i < AIRPLAY_MAX_SESSIONS; i++) {
        if (srv->sessions[i].active &&
            srv->sessions[i].rtsp.state == RTSP_STATE_RECORDING) {
            ret = rtsp_send_touch(&srv->sessions[i].rtsp, x, y, touch_type);
            break;
        }
    }
    pthread_mutex_unlock(&srv->sessions_lock);
    return ret;
}

void airplay_server_stop(airplay_server_t *srv)
{
    if (!srv) return;
    srv->running = 0;

    /* Close listeners to unblock accept() */
    if (srv->rtsp_listen_fd >= 0) {
        shutdown(srv->rtsp_listen_fd, SHUT_RDWR);
        close(srv->rtsp_listen_fd);
        srv->rtsp_listen_fd = -1;
    }
    if (srv->mirror_listen_fd >= 0) {
        shutdown(srv->mirror_listen_fd, SHUT_RDWR);
        close(srv->mirror_listen_fd);
        srv->mirror_listen_fd = -1;
    }

    /* Join all active session threads, then clean up */
    for (int i = 0; i < AIRPLAY_MAX_SESSIONS; i++) {
        if (srv->sessions[i].active) {
            /* Close RTSP fd to unblock rtsp_read_request() */
            if (srv->sessions[i].rtsp_client_fd >= 0) {
                shutdown(srv->sessions[i].rtsp_client_fd, SHUT_RDWR);
            }
            pthread_join(srv->sessions[i].rtsp_thread, NULL);
        }
    }
    pthread_mutex_lock(&srv->sessions_lock);
    for (int i = 0; i < AIRPLAY_MAX_SESSIONS; i++) {
        if (srv->sessions[i].active) {
            session_cleanup(&srv->sessions[i]);
        }
    }
    pthread_mutex_unlock(&srv->sessions_lock);

    /* Unregister mDNS */
    if (srv->mdns) {
        airplay_mdns_unregister(srv->mdns);
        srv->mdns = NULL;
    }

    pthread_mutex_destroy(&srv->sessions_lock);
    pthread_mutex_destroy(&srv->mirror_accept_lock);
    printf("server: AirPlay receiver stopped\n");
}

/*
 * carplay_input.c — Accept H.264/H.265 video from the CarPlay AirPlay receiver
 *
 * Internal design:
 *
 *   Two frame buffers (ping-pong):
 *     buf[0] and buf[1] — each CARPLAY_MAX_FRAME bytes.
 *
 *   The receiver thread writes into buf[write_idx], then atomically
 *   swaps write_idx and sets g_new_frame = 1.
 *
 *   carplay_input_get_frame() copies the pointer and codec under a mutex,
 *   clears g_new_frame, and returns.
 *
 *   Frame read protocol:
 *     1. Read 4-byte big-endian length prefix.
 *     2. Read exactly `length` bytes of NAL data.
 *     3. Run nal_detect_codec() to identify H.264 vs H.265.
 *     4. Store in buffer.
 */
#include "carplay_input.h"
#include "nal_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ---- Internal state ---- */

static int              g_server_fd = -1;
static int              g_client_fd = -1;
static pthread_t        g_thread;
static volatile int     g_running  = 0;

/* Double-buffered frames */
static uint8_t         *g_buf[2]   = { NULL, NULL };
static size_t           g_buf_len[2] = { 0, 0 };
static input_codec_t    g_codec[2];
static int              g_write_idx = 0;  /* thread writes here */
static int              g_read_idx  = 1;  /* consumer reads here */
static int              g_new_frame = 0;  /* 1 = unread frame in g_read_idx */

static pthread_mutex_t  g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- Helpers ---- */

/* Read exactly `n` bytes from fd; returns 0 on success, -1 on error/eof */
static int read_exact(int fd, uint8_t *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = recv(fd, buf + done, n - done, 0);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

/* ---- Receiver thread ---- */

static void *carplay_recv_thread(void *arg)
{
    (void)arg;

    while (g_running) {
        /* Wait for AirPlay receiver to connect */
        printf("carplay_input: waiting for AirPlay receiver on %s\n",
               CARPLAY_SOCK_PATH);

        int fd = accept(g_server_fd, NULL, NULL);
        if (fd < 0) {
            if (g_running) perror("carplay_input: accept");
            continue;
        }
        printf("carplay_input: AirPlay receiver connected\n");

        pthread_mutex_lock(&g_mutex);
        g_client_fd = fd;
        pthread_mutex_unlock(&g_mutex);

        /* Receive frames */
        while (g_running) {
            /* Read 4-byte length prefix (network byte order) */
            uint8_t len_buf[4];
            if (read_exact(fd, len_buf, 4) < 0)
                break;

            uint32_t frame_len = ((uint32_t)len_buf[0] << 24) |
                                 ((uint32_t)len_buf[1] << 16) |
                                 ((uint32_t)len_buf[2] <<  8) |
                                 ((uint32_t)len_buf[3]);

            if (frame_len == 0 || frame_len > CARPLAY_MAX_FRAME) {
                fprintf(stderr,
                        "carplay_input: bad frame length %u, skipping\n",
                        frame_len);
                break;
            }

            /* Read frame data into the current write buffer */
            int wi = g_write_idx;

            if (read_exact(fd, g_buf[wi], frame_len) < 0)
                break;

            g_buf_len[wi] = frame_len;

            /* Auto-detect codec */
            int detected = nal_detect_codec(g_buf[wi], frame_len);
            g_codec[wi] = (detected == CODEC_H265) ? CODEC_H265 : CODEC_H264;

            /* Swap buffers and signal new frame */
            pthread_mutex_lock(&g_mutex);
            g_read_idx  = wi;
            g_write_idx = wi ^ 1;  /* toggle 0↔1 */
            g_new_frame = 1;
            pthread_mutex_unlock(&g_mutex);
        }

        pthread_mutex_lock(&g_mutex);
        g_client_fd = -1;
        pthread_mutex_unlock(&g_mutex);

        close(fd);
        printf("carplay_input: AirPlay receiver disconnected\n");
    }

    return NULL;
}

/* ---- Public API ---- */

int carplay_input_init(void)
{
    /* Allocate double-buffered frame storage */
    g_buf[0] = malloc(CARPLAY_MAX_FRAME);
    g_buf[1] = malloc(CARPLAY_MAX_FRAME);
    if (!g_buf[0] || !g_buf[1]) {
        fprintf(stderr, "carplay_input: buffer allocation failed\n");
        free(g_buf[0]);
        free(g_buf[1]);
        return -1;
    }

    /* Create Unix domain server socket */
    unlink(CARPLAY_SOCK_PATH);

    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("carplay_input: socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CARPLAY_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("carplay_input: bind");
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }
    if (listen(g_server_fd, 1) < 0) {
        perror("carplay_input: listen");
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    g_running = 1;
    g_new_frame = 0;
    g_write_idx = 0;
    g_read_idx  = 1;

    if (pthread_create(&g_thread, NULL, carplay_recv_thread, NULL) != 0) {
        perror("carplay_input: pthread_create");
        g_running = 0;
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    printf("carplay_input: listening on %s\n", CARPLAY_SOCK_PATH);
    return 0;
}

int carplay_input_get_frame(const uint8_t **data, size_t *len,
                             input_codec_t *codec)
{
    pthread_mutex_lock(&g_mutex);

    if (!g_new_frame) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }

    *data     = g_buf[g_read_idx];
    *len      = g_buf_len[g_read_idx];
    *codec    = g_codec[g_read_idx];
    g_new_frame = 0;

    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void carplay_input_destroy(void)
{
    g_running = 0;

    /* Close server to unblock accept() in thread */
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }

    /* Close client if connected */
    pthread_mutex_lock(&g_mutex);
    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
    }
    pthread_mutex_unlock(&g_mutex);

    pthread_join(g_thread, NULL);

    free(g_buf[0]); g_buf[0] = NULL;
    free(g_buf[1]); g_buf[1] = NULL;

    unlink(CARPLAY_SOCK_PATH);
    printf("carplay_input: destroyed\n");
}
